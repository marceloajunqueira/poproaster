/**
 * @file display_panel.c
 * @brief Physical display bring-up implementation (see header).
 *
 * Bus: NV3041A over QSPI (CS/SCK/D0-D3, no D/C line). This uses a custom,
 * from-scratch driver (ui_display/nv3041_qspi.h) instead of the generic
 * esp_lcd_panel_io_spi + eric-c-e/esp_lcd_nv3041 vendor component, because
 * neither of those support the wire protocol this exact 6-pin QSPI wiring
 * actually requires (SPI hardware cmd=0x02/0x32 + address-field framing,
 * confirmed against the community's working Arduino_GFX Arduino_ESP32QSPI
 * implementation - see nv3041_qspi.c for the full rationale). LVGL is wired
 * up manually (raw LVGL8 disp_drv API) since esp_lvgl_port's lvgl_port_add_disp()
 * requires esp_lcd_panel_io_handle_t/esp_lcd_panel_handle_t types, which this
 * custom driver does not use.
 */
#include "driver/ledc.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"
#include "hal/touch_driver.h"
#include "ui_display/nv3041_qspi.h"
#include "ui_display/display_panel.h"

static const char *TAG = "display_panel";

/* Partial LVGL draw buffer, in display lines - kept in PSRAM (8MB on this
 * board, reserved for HMI use). A full-frame single-shot buffer + full_refresh
 * was tried earlier and caused an LVGL task hang; chunked partial refresh is
 * the safe approach. */
#define DISPLAY_LINE_BUF_LINES 40

/* Backlight is driven on LEDC timer/channel 0, reserved for this purpose
 * (see hal/fan_pwm.h, which defaults to timer/channel 1 to avoid conflict). */
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_FREQ_HZ 5000
#define BACKLIGHT_LEDC_RES     LEDC_TIMER_10_BIT
#define BACKLIGHT_DEFAULT_DUTY_PCT 80

static lv_disp_drv_t s_disp_drv;
static lv_disp_draw_buf_t s_disp_draw_buf;
static lv_display_t *s_lv_display;

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_LEDC_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t max_duty = (1u << BACKLIGHT_LEDC_RES) - 1u;
    ledc_channel_config_t channel_cfg = {
        .gpio_num = BOARD_DISPLAY_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = (max_duty * BACKLIGHT_DEFAULT_DUTY_PCT) / 100,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_cfg);
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    /* LVGL stores RGB565 colors in native (little-endian) byte order by
     * default; SPI TFT controllers expect big-endian (MSB first) - swap
     * bytes in place before sending (equivalent to Arduino_GFX's MSB_*_SET
     * helpers / LVGL's LV_COLOR_16_SWAP, done manually here). */
    uint8_t *bytes = (uint8_t *)color_p;
    size_t px_count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < px_count; i++) {
        uint8_t tmp = bytes[i * 2];
        bytes[i * 2] = bytes[i * 2 + 1];
        bytes[i * 2 + 1] = tmp;
    }

    static int s_flush_log_count = 0;
    esp_err_t draw_err = nv3041_qspi_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    if (s_flush_log_count < 10 || draw_err != ESP_OK) {
        ESP_LOGI(TAG, "flush #%d area=(%d,%d)-(%d,%d) %dx%d -> %s",
                 s_flush_log_count, (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2,
                 (int)w, (int)h, esp_err_to_name(draw_err));
    }
    s_flush_log_count++;
    lv_disp_flush_ready(drv);
}

esp_err_t ui_display_panel_init(void)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nv3041_qspi_init();
    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* PSRAM-backed partial draw buffers (8MB PSRAM on this board, reserved for HMI use). */
    size_t buf_px = (size_t)BOARD_DISPLAY_WIDTH_PX * DISPLAY_LINE_BUF_LINES;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "LVGL draw buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_disp_draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BOARD_DISPLAY_WIDTH_PX;
    s_disp_drv.ver_res = BOARD_DISPLAY_HEIGHT_PX;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_draw_buf;
    s_lv_display = lv_disp_drv_register(&s_disp_drv);
    if (s_lv_display == NULL) {
        ESP_LOGE(TAG, "lv_disp_drv_register failed");
        return ESP_FAIL;
    }

    /* Material-Design-inspired dark theme (dark surfaces save pixels/power
     * and are easier on the eyes in a kitchen/roasting environment). Primary
     * accent #FF9746, matching the web UI's theme. */
    lv_theme_t *theme = lv_theme_default_init(s_lv_display, lv_color_hex(0xFF9746), lv_color_hex(0x6200EE),
                                               true, LV_FONT_DEFAULT);
    lv_disp_set_theme(s_lv_display, theme);

    esp_lcd_touch_handle_t touch_handle = touch_driver_get_handle();
    if (touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_lv_display,
            .handle = touch_handle,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed - continuing without touch input");
        }
    } else {
        ESP_LOGW(TAG, "No touch handle available - call touch_driver_init() before ui_display_panel_init()");
    }

    ESP_LOGI(TAG, "Display panel init OK (%dx%d, %s, backlight=%d%%)",
             BOARD_DISPLAY_WIDTH_PX, BOARD_DISPLAY_HEIGHT_PX, BOARD_DISPLAY_DRIVER_NAME,
             BACKLIGHT_DEFAULT_DUTY_PCT);
    return ESP_OK;
}

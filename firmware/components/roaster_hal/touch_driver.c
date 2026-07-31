/**
 * @file touch_driver.c
 * @brief GT911 touch controller bring-up via esp_lcd_touch_gt911.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "hal/touch_driver.h"

static const char *TAG = "touch_driver";

#define TOUCH_I2C_PORT       I2C_NUM_0
#define TOUCH_INIT_MAX_RETRIES 3
#define TOUCH_INIT_RETRY_DELAY_MS 100

/* Operator-reported bug: touch occasionally goes completely dead mid-session
 * (screen keeps updating, taps do nothing) until power-cycled - the GT911
 * itself wedges (or the I2C transaction starts erroring out) with no way to
 * recover on its own. If read_data() keeps failing for this long, pulse the
 * chip's own RST line (same pulse esp_lcd_touch_gt911 does at cold boot) to
 * reboot just the touch controller, in place, without touching the
 * esp_lcd_touch_handle_t pointer LVGL already holds. */
#define TOUCH_RECOVERY_FAIL_MS      1500
#define TOUCH_RECOVERY_COOLDOWN_MS  3000
#define TOUCH_RESET_PULSE_MS        10
#define TOUCH_RESET_BOOT_DELAY_MS   60

/* Second, likely more common failure mode: the GT911's read_data still
 * returns ESP_OK every poll, but the reported point never changes because
 * the driver's own write that clears the chip's "buffer ready" flag
 * (touch_gt911_i2c_write() in the >5-or-0-points and out-of-range branches
 * of esp_lcd_touch_gt911_read_data()) failed and was never retried - the
 * flag stays set on the chip, so every subsequent read replays the same
 * stale (possibly "pressed") point forever. No real tap or on-screen
 * long-press in this UI holds a single coordinate this long. */
#define TOUCH_STUCK_PRESS_MS        4000

/* If the chip doesn't answer at all within the fast boot-time retries
 * below, keep trying in the background at a slower pace indefinitely -
 * this hardware sits right next to a hot air tunnel, so a marginal
 * connector/solder joint can plausibly come and go with thermal cycling;
 * a background retry means it can recover on its own without a reboot. */
#define TOUCH_BACKGROUND_RETRY_MS   3000
#define TOUCH_RETRY_TASK_STACK      3072
#define TOUCH_RETRY_TASK_PRIO       2

static i2c_master_bus_handle_t s_i2c_bus_handle;
static esp_lcd_touch_handle_t s_touch_handle;
static esp_lcd_panel_io_handle_t s_touch_io_handle;
static esp_lcd_touch_config_t s_touch_cfg;
static touch_driver_ready_cb_t s_ready_cb;

static esp_err_t (*s_orig_read_data)(esp_lcd_touch_handle_t tp);
static bool s_read_failing;
static int64_t s_fail_start_ms;
static int64_t s_last_recovery_ms;
static uint32_t s_recovery_count;
static bool s_stuck_pressed;
static int64_t s_stuck_since_ms;
static uint16_t s_stuck_x, s_stuck_y;

/* Reproduces touch_gt911_reset()'s own pulse (private to that component) so
 * a wedged chip reboots into the same state it would after a cold power-on. */
static void touch_gt911_recover(esp_lcd_touch_handle_t tp)
{
    s_recovery_count++;
    ESP_LOGE(TAG, "GT911 unresponsive for >%dms - pulsing reset to recover (recovery #%u)",
              TOUCH_RECOVERY_FAIL_MS, (unsigned)s_recovery_count);

    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset);
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_PULSE_MS));
        gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset);
        vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_BOOT_DELAY_MS));
    }

    /* Don't leave a stale/stuck coordinate behind for LVGL to keep treating
     * as a held-down press while the chip reboots. */
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t touch_gt911_read_data_with_recovery(esp_lcd_touch_handle_t tp)
{
    esp_err_t err = s_orig_read_data(tp);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (err != ESP_OK) {
        if (!s_read_failing) {
            s_read_failing = true;
            s_fail_start_ms = now_ms;
            return err;
        }
        if ((now_ms - s_fail_start_ms) >= TOUCH_RECOVERY_FAIL_MS &&
            (now_ms - s_last_recovery_ms) >= TOUCH_RECOVERY_COOLDOWN_MS) {
            touch_gt911_recover(tp);
            s_last_recovery_ms = now_ms;
            s_fail_start_ms = now_ms; /* Wait for the cooldown before retrying if still bad. */
        }
        return err;
    }
    s_read_failing = false;

    /* Phantom-press check - see TOUCH_STUCK_PRESS_MS above. */
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t points = tp->data.points;
    uint16_t x = (points > 0) ? tp->data.coords[0].x : 0;
    uint16_t y = (points > 0) ? tp->data.coords[0].y : 0;
    portEXIT_CRITICAL(&tp->data.lock);

    if (points == 0) {
        s_stuck_pressed = false;
        return ESP_OK;
    }
    if (!s_stuck_pressed || x != s_stuck_x || y != s_stuck_y) {
        s_stuck_pressed = true;
        s_stuck_since_ms = now_ms;
        s_stuck_x = x;
        s_stuck_y = y;
        return ESP_OK;
    }
    if ((now_ms - s_stuck_since_ms) >= TOUCH_STUCK_PRESS_MS &&
        (now_ms - s_last_recovery_ms) >= TOUCH_RECOVERY_COOLDOWN_MS) {
        ESP_LOGE(TAG, "GT911 stuck reporting a phantom press at (%u,%u) for >%dms - recovering",
                  (unsigned)x, (unsigned)y, TOUCH_STUCK_PRESS_MS);
        touch_gt911_recover(tp);
        s_last_recovery_ms = now_ms;
        s_stuck_pressed = false;
    }
    return ESP_OK;
}

/* One attempt at esp_lcd_touch_new_i2c_gt911() over the already-created I2C
 * bus/panel IO, plus splicing in the read_data recovery wrapper on success -
 * shared by the fast boot-time retry loop and the slower indefinite
 * background retry task below. */
static esp_err_t touch_gt911_try_bring_up(void)
{
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(s_touch_io_handle, &s_touch_cfg, &s_touch_handle);
    if (err != ESP_OK) {
        return err;
    }
    s_orig_read_data = s_touch_handle->read_data;
    s_touch_handle->read_data = touch_gt911_read_data_with_recovery;
    return ESP_OK;
}

static void touch_retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_BACKGROUND_RETRY_MS));
        esp_err_t err = touch_gt911_try_bring_up();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GT911 came up on a background retry - touch is back");
            if (s_ready_cb != NULL) {
                s_ready_cb(s_touch_handle);
            }
            break;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t touch_driver_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.dev_addr = BOARD_TOUCH_I2C_ADDR;
    err = esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &tp_io_config, &s_touch_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed: %s", esp_err_to_name(err));
        return err;
    }

    s_touch_cfg = (esp_lcd_touch_config_t){
        .x_max = BOARD_DISPLAY_WIDTH_PX,
        .y_max = BOARD_DISPLAY_HEIGHT_PX,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* GT911 occasionally misses its first I2C transaction right after power-up
     * (observed intermittently on hardware) - retry a few times with a short
     * delay before giving up, instead of failing the whole boot on one glitch. */
    for (int attempt = 1; attempt <= TOUCH_INIT_MAX_RETRIES; attempt++) {
        err = touch_gt911_try_bring_up();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_lcd_touch_new_i2c_gt911 attempt %d/%d failed: %s",
                 attempt, TOUCH_INIT_MAX_RETRIES, esp_err_to_name(err));
        if (attempt < TOUCH_INIT_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_INIT_RETRY_DELAY_MS));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 failed after %d attempts: %s - retrying in the "
                  "background every %dms instead of giving up for the rest of this boot",
                  TOUCH_INIT_MAX_RETRIES, esp_err_to_name(err), TOUCH_BACKGROUND_RETRY_MS);
        xTaskCreate(touch_retry_task, "touch_retry", TOUCH_RETRY_TASK_STACK, NULL, TOUCH_RETRY_TASK_PRIO, NULL);
        return err;
    }

    ESP_LOGI(TAG, "GT911 touch init OK (SDA=%d SCL=%d RST=%d INT=%d addr=0x%02X)",
             BOARD_TOUCH_PIN_SDA, BOARD_TOUCH_PIN_SCL, BOARD_TOUCH_PIN_RST,
             BOARD_TOUCH_PIN_INT, BOARD_TOUCH_I2C_ADDR);
    return ESP_OK;
}

esp_lcd_touch_handle_t touch_driver_get_handle(void)
{
    return s_touch_handle;
}

uint32_t touch_driver_get_recovery_count(void)
{
    return s_recovery_count;
}

void touch_driver_set_ready_callback(touch_driver_ready_cb_t cb)
{
    s_ready_cb = cb;
    if (s_touch_handle != NULL && cb != NULL) {
        cb(s_touch_handle); /* Touch was already up by the time this was registered. */
    }
}

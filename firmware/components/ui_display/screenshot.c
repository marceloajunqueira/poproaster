/**
 * @file screenshot.c
 * @brief See header.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_display/screenshot.h"
#include "ui_display/widgets/nav_shell.h"

static const char *TAG = "screenshot";

#define LVGL_LOCK_TIMEOUT_MS 2000
/* Lets the new tab's refresh timers repaint real data before the snapshot. */
#define TAB_SETTLE_MS 600

static esp_err_t take_snapshot(ui_display_screenshot_t *out)
{
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_update_layout(scr);

    /* ~255KB for 480x272 RGB565 - far above LVGL's own 96KB pool, so the buffer comes from PSRAM. */
    uint32_t need = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    void *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "No memory for %u byte snapshot", (unsigned)need);
        return ESP_ERR_NO_MEM;
    }

    lv_img_dsc_t dsc;
    lv_res_t res = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, need);
    lvgl_port_unlock();

    if (res != LV_RES_OK) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    out->data = buf;
    out->width = dsc.header.w;
    out->height = dsc.header.h;
    return ESP_OK;
}

esp_err_t ui_display_screenshot_capture(int tab, ui_display_screenshot_t *out)
{
    memset(out, 0, sizeof(*out));

    nav_shell_tab_t original = NAV_SHELL_TAB_COUNT;
    bool switched = false;

    if (tab >= 0) {
        if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
            return ESP_ERR_TIMEOUT;
        }
        original = nav_shell_get_current_tab();
        esp_err_t err = ESP_OK;
        if ((nav_shell_tab_t)tab != original) {
            err = nav_shell_select_tab((nav_shell_tab_t)tab);
            switched = (err == ESP_OK);
        }
        lvgl_port_unlock();
        if (err != ESP_OK) {
            return err;
        }
        if (switched) {
            vTaskDelay(pdMS_TO_TICKS(TAB_SETTLE_MS));
        }
    }

    esp_err_t result = take_snapshot(out);

    if (switched && original < NAV_SHELL_TAB_COUNT && lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nav_shell_select_tab(original);
        lvgl_port_unlock();
    }
    return result;
}

void ui_display_screenshot_row_bgr(const ui_display_screenshot_t *shot, int y, uint8_t *out)
{
    const lv_color_t *px = (const lv_color_t *)shot->data + (size_t)y * shot->width;
    for (int x = 0; x < shot->width; x++) {
        uint32_t c = lv_color_to32(px[x]);
        out[3 * x] = c & 0xFF;
        out[3 * x + 1] = (c >> 8) & 0xFF;
        out[3 * x + 2] = (c >> 16) & 0xFF;
    }
}

void ui_display_screenshot_free(ui_display_screenshot_t *shot)
{
    heap_caps_free(shot->data);
    memset(shot, 0, sizeof(*shot));
}

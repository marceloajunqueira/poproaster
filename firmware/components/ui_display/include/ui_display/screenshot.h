/**
 * @file screenshot.h
 * @brief Captures the active LVGL screen (via lv_snapshot) into a PSRAM buffer, for documentation screenshots.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *data; /* Native-format pixel buffer; opaque to callers. */
    uint16_t width;
    uint16_t height;
} ui_display_screenshot_t;

/** Captures the current screen. `tab` < 0 captures whatever is shown; otherwise it is a
 * nav_shell_tab_t to switch to first (and restore afterwards). Free with ui_display_screenshot_free(). */
esp_err_t ui_display_screenshot_capture(int tab, ui_display_screenshot_t *out);

/** Converts row `y` (0 = top) to `width * 3` bytes of B,G,R. */
void ui_display_screenshot_row_bgr(const ui_display_screenshot_t *shot, int y, uint8_t *out);

void ui_display_screenshot_free(ui_display_screenshot_t *shot);

#ifdef __cplusplus
}
#endif

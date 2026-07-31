/**
 * @file display_panel.h
 * @brief Physical display bring-up: QSPI NV3041A panel + backlight + LVGL
 *        port + GT911 touch input wiring.
 *
 * This is hardware bring-up infrastructure (not a UI screen). Screens
 * (dashboard, profile editor, etc. - later tasks T021+) are built on top of
 * the LVGL display this module creates; call ui_display_panel_init() once
 * during boot, after touch_driver_init() (see main.c).
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

/**
 * Brings up the QSPI bus + NV3041A panel + backlight PWM, initializes the
 * LVGL port task, registers the display and the already-initialized GT911
 * touch input (touch_driver_init() MUST have been called first).
 */
esp_err_t ui_display_panel_init(void);

/**
 * Attaches a GT911 touch handle to the already-running display - used both
 * internally by ui_display_panel_init() and by main.c's
 * touch_driver_ready_callback for when the GT911 only comes up later, via
 * touch_driver.c's background retry task (see touch_driver_set_ready_callback()).
 * Safe to call more than once; a no-op if touch is already attached.
 */
esp_err_t ui_display_panel_attach_touch(esp_lcd_touch_handle_t touch_handle);

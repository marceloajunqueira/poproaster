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

/**
 * Brings up the QSPI bus + NV3041A panel + backlight PWM, initializes the
 * LVGL port task, registers the display and the already-initialized GT911
 * touch input (touch_driver_init() MUST have been called first).
 */
esp_err_t ui_display_panel_init(void);

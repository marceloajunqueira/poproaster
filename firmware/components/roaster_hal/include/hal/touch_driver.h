/**
 * @file touch_driver.h
 * @brief GT911 capacitive touch controller integration (confirmed hardware).
 *
 * Pins come from board_config.h (BOARD_TOUCH_PIN_*, BOARD_TOUCH_I2C_ADDR),
 * fixed by the JC4827W543 board itself (not user-configurable, unlike the
 * external peripheral GPIOs).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_touch.h"

/** Invoked once touch becomes available - either from touch_driver_set_ready_callback()
 * itself (if touch_driver_init() already succeeded) or later, from a
 * background task, if the GT911 only came up after boot-time retries gave
 * up (see touch_driver_init()). Return value is ignored (matches
 * ui_display_panel_attach_touch()'s signature so it can be passed directly). */
typedef esp_err_t (*touch_driver_ready_cb_t)(esp_lcd_touch_handle_t handle);

/**
 * Initializes the I2C bus and the GT911 touch controller, producing an
 * esp_lcd_touch_handle_t usable by esp_lvgl_port for input registration.
 *
 * Also installs an in-place recovery wrapper around the handle's read_data
 * callback (see touch_driver.c) - operator-reported bug: the GT911
 * occasionally stops responding entirely mid-session (screen keeps
 * redrawing fine, touch just goes dead) until the board is power-cycled.
 * The wrapper pulses the RST line to reboot the controller after it's been
 * failing continuously for a while, without needing a reset of the whole
 * board or re-registering the LVGL input device.
 *
 * If the GT911 doesn't answer at all during boot (this function then
 * returns an error), a background task keeps retrying indefinitely at a
 * slower pace - see touch_driver_set_ready_callback() to be notified if/when
 * that succeeds, so the caller can still hot-plug touch into the UI later.
 */
esp_err_t touch_driver_init(void);

/** Returns the esp_lcd_touch handle for LVGL/esp_lvgl_port registration, or NULL if not initialized. */
esp_lcd_touch_handle_t touch_driver_get_handle(void);

/**
 * Registers a callback fired once touch is available - immediately, if
 * touch_driver_init() already succeeded, or later (from the background
 * retry task's context, not the caller's) if it didn't. Only one callback
 * is kept; registering again replaces it.
 */
void touch_driver_set_ready_callback(touch_driver_ready_cb_t cb);

/** Number of times the GT911 has been auto-recovered (reset-pulsed) since boot - for diagnostics. */
uint32_t touch_driver_get_recovery_count(void);

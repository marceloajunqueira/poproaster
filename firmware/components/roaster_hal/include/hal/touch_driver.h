/**
 * @file touch_driver.h
 * @brief GT911 capacitive touch controller integration (confirmed hardware).
 *
 * Pins come from board_config.h (BOARD_TOUCH_PIN_*, BOARD_TOUCH_I2C_ADDR),
 * fixed by the JC4827W543 board itself (not user-configurable, unlike the
 * external peripheral GPIOs).
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

/**
 * Initializes the I2C bus and the GT911 touch controller, producing an
 * esp_lcd_touch_handle_t usable by esp_lvgl_port for input registration.
 */
esp_err_t touch_driver_init(void);

/** Returns the esp_lcd_touch handle for LVGL/esp_lvgl_port registration, or NULL if not initialized. */
esp_lcd_touch_handle_t touch_driver_get_handle(void);

/**
 * @file fan_pwm.h
 * @brief Fan PWM control driver (LEDC-based).
 *
 * Uses BOARD_PERIPH_FAN_PWM_GPIO / LEDC timer/channel/frequency from
 * board_config.h. LEDC channel/timer 0 is reserved for the display backlight
 * on JC4827W543 reference firmware, so this defaults to timer/channel 1.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/** Initializes the LEDC timer/channel driving the fan PWM output. */
esp_err_t fan_pwm_init(void);

/**
 * Sets the fan speed as a percentage (0-100).
 *
 * NOTE: Like ssr_heater_set_duty_pct(), this does not enforce the 30% fixed
 * safety floor during heating (FR-004) - only the Safety Manager may call
 * this with a value below the floor while the heater is active is REJECTED
 * upstream; this driver only stores the electrical PWM duty being requested.
 */
esp_err_t fan_pwm_set_pct(uint8_t pct);

/** Returns the last commanded fan speed percentage. */
uint8_t fan_pwm_get_pct(void);

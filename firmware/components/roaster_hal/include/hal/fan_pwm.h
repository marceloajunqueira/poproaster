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
 * - Any nonzero request below the fan's physical minimum operating duty
 *   (65%) is clamped up to that floor - the motor doesn't reliably spin at
 *   lower duty cycles.
 * - Turning the fan ON from a full stop (0 -> nonzero) ramps smoothly to the
 *   target duty over ~3 seconds (soft start, avoids a hard power-supply
 *   inrush); adjusting an already-running fan's speed, or turning it off,
 *   is instantaneous.
 *
 * NOTE: Like ssr_heater_set_duty_pct(), this does not enforce the safety-
 * critical fan floor during heating (FR-004) - only the Safety Manager may
 * enforce that; this driver only applies the two hardware-level rules above
 * to whatever duty is requested.
 */
esp_err_t fan_pwm_set_pct(uint8_t pct);

/** Returns the last commanded fan speed percentage. */
uint8_t fan_pwm_get_pct(void);

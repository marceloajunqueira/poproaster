/**
 * @file ssr_heater.h
 * @brief SSR (solid-state relay) heater control driver.
 *
 * Single digital output pin (BOARD_PERIPH_SSR_HEATER_GPIO). Duty-cycle
 * (time-proportioning) control is implemented in software here since an SSR
 * is a simple on/off switch, not a PWM-capable device - fast switching would
 * wear it out and cause flicker on the mains side.
 */
#pragma once

#include "esp_err.h"

/** Initializes the SSR control GPIO (starts OFF). */
esp_err_t ssr_heater_init(void);

/**
 * Sets the desired heater duty cycle as a percentage (0-100).
 * 0 turns the SSR fully off immediately; values are applied via
 * time-proportioning over ssr_heater_get_window_ms().
 *
 * NOTE: This function does not itself enforce safety rules (fan floor,
 * absolute temperature cutoff, etc.) - callers MUST go through the Safety
 * Manager (firmware/components/safety/safety_manager.c), which is the only
 * component allowed to call this directly with a validated duty cycle.
 */
esp_err_t ssr_heater_set_duty_pct(uint8_t duty_pct);

/** Returns the last commanded duty cycle percentage. */
uint8_t ssr_heater_get_duty_pct(void);

/** Immediately forces the SSR off, bypassing the duty-cycle window (for Emergency Stop / hard cutoffs). */
esp_err_t ssr_heater_force_off(void);

/** Returns the time-proportioning window length in milliseconds (e.g. 1000-2000ms is typical for SSR). */
uint32_t ssr_heater_get_window_ms(void);

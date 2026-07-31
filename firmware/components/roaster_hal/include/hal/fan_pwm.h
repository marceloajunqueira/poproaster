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

/** Number of discrete fan levels, including 0 (off): 0,1,2,3. */
#define FAN_PWM_LEVEL_COUNT 4
/** Highest valid discrete fan level (3 = 100%). */
#define FAN_PWM_LEVEL_MAX 3

/** The one definition of the level->percent table; see fan_pwm.c for why the band is high and narrow. */
#define FAN_PWM_LEVEL_PCT_LIST 0, 80, 90, 100

#define FAN_PWM_STRINGIFY_(...) #__VA_ARGS__
#define FAN_PWM_STRINGIFY(...) FAN_PWM_STRINGIFY_(__VA_ARGS__)
#define FAN_PWM_LEVEL_MAX_STR FAN_PWM_STRINGIFY(FAN_PWM_LEVEL_MAX)
/** Same table as a JS/JSON array literal, so web UIs can't drift out of sync with the HAL. */
#define FAN_PWM_LEVEL_PCT_JSON "[" FAN_PWM_STRINGIFY(FAN_PWM_LEVEL_PCT_LIST) "]"

/** Initializes the LEDC timer/channel driving the fan PWM output. */
esp_err_t fan_pwm_init(void);

/**
 * Sets the fan speed as a percentage (0-100).
 *
 * - Operator report: the fan motor only behaves predictably at a handful of
 *   discrete speeds, not arbitrary percentages - any nonzero request is
 *   snapped to the nearest of the 3 fixed levels (80/90/100%, see
 *   fan_pwm_level_to_pct()); a request that would round down to "off" is
 *   instead raised to the lowest nonzero level (80%) - a deliberate nonzero
 *   request must never silently become 0%.
 * - Turning the fan ON from a full stop (0 -> nonzero) ramps smoothly to the
 *   target duty over ~2 seconds (soft start, avoids a hard power-supply
 *   inrush); adjusting an already-running fan's speed, or turning it off,
 *   is instantaneous.
 *
 * NOTE: Like ssr_heater_set_duty_pct(), this does not enforce the safety-
 * critical fan floor during heating (FR-004) - only the Safety Manager may
 * enforce that; this driver only applies the two hardware-level rules above
 * to whatever duty is requested.
 */
esp_err_t fan_pwm_set_pct(uint8_t pct);

/**
 * Returns the CURRENT REAL fan speed percentage - while a soft-start ramp
 * is in progress, this is a live interpolated value (matching the
 * hardware's own linear fade), not the eventual target. Used by the Safety
 * Manager's fan-floor check so the heater can't turn on before the fan has
 * ACTUALLY reached the required speed.
 */
uint8_t fan_pwm_get_pct(void);

/**
 * Returns the last commanded fan speed TARGET percentage (post-level-
 * quantization) - unlike fan_pwm_get_pct(), this does NOT interpolate
 * during an in-progress ramp; it's always the final value the fan is
 * heading toward. Intended for callers that need to know "what did I ask
 * for" regardless of whether the physical ramp has finished yet (e.g.
 * profile_curve_follower.c's manual-override detection, which must not
 * mistake its own in-progress ramp for an external override).
 */
uint8_t fan_pwm_get_target_pct(void);

/**
 * Immediately cuts the fan to 0%, bypassing the normal soft-start/level
 * logic entirely (no fade, no quantization) - for the Emergency Stop path
 * ONLY (safety_manager_emergency_stop()), which per operator requirement
 * must cut power to everything at once, unlike other critical alarms that
 * deliberately leave the fan running for continued safe airflow.
 */
esp_err_t fan_pwm_force_off(void);

/**
 * Converts a discrete fan level (0-FAN_PWM_LEVEL_MAX) to its corresponding
 * PWM percentage: 0=0%, 1=60%, 2=70%, 3=80%, 4=90%, 5=100%. Levels above
 * FAN_PWM_LEVEL_MAX are clamped to it.
 */
uint8_t fan_pwm_level_to_pct(uint8_t level);

/**
 * Converts a raw percentage to the nearest discrete fan level (0-5) - used
 * to display/reconstruct a level from an already-stored/legacy percentage
 * value (e.g. an older profile segment that predates the discrete-level
 * model).
 */
uint8_t fan_pwm_pct_to_level(uint8_t pct);

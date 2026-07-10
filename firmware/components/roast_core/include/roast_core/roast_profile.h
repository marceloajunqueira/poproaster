/**
 * @file roast_profile.h
 * @brief T030: Roast Profile model - a reusable roast "recipe" made of a
 *        sequence of setpoints (duration + target bean temperature + target
 *        fan speed). The Roast dashboard runs whichever profile is selected
 *        via the Presets tab: its target BT/Fan curves are plotted on the
 *        live chart (dashed) against the actual measured values (solid),
 *        and the chart's timeline spans exactly the profile's total
 *        duration (sum of every setpoint's duration).
 *
 * Each setpoint is a flat STEP, not a ramp: roast_profile_get_target_*()
 * simply returns whichever segment `elapsed_s` currently falls into's own
 * configured target, held constant for that segment's whole duration - the
 * transition at a segment boundary is immediate (per operator requirement:
 * no smoothing/interpolation between one segment's target and the next).
 *
 * There is deliberately no configurable heater-power field: the operator
 * only ever picks the target BEAN TEMPERATURE, and the firmware works out
 * the actual heater duty cycle needed to track it via a closed-loop PID
 * controller (roast_core/heater_pid.h), using the live BT sensor as
 * feedback - see profile_curve_follower.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROAST_PROFILE_NAME_MAX_LEN 32
#define ROAST_PROFILE_MAX_POINTS 20

/** FR-004: fan may never drop below this while the heater could be active - normal (non-Cooling) segments must keep target_fan_pct at/above this floor (enforced both by the profile editor UI and, independently, by the Safety Manager at the point commands are actually applied). */
#define ROAST_PROFILE_FAN_MIN_PCT 30

/** Cooling segments always use these two fixed values - not editable/stored as a choice, just implied by is_cooling (see below). */
#define ROAST_PROFILE_COOLING_TEMP_C 0.0f
#define ROAST_PROFILE_COOLING_FAN_PCT 100

typedef struct {
    uint32_t duration_s;      /* How long this segment lasts. */
    float target_temp_c;      /* BT target to be reached by the end of this segment - drives the closed-loop heater PID (heater_pid.h) and is also plotted as the dashed target curve. Fixed at ROAST_PROFILE_COOLING_TEMP_C when is_cooling. */
    uint8_t target_fan_pct;   /* Fan target to be reached by the end of this segment - actionable open-loop setpoint (T034 curve follower), must be >= ROAST_PROFILE_FAN_MIN_PCT unless is_cooling (fixed at ROAST_PROFILE_COOLING_FAN_PCT instead). */
    bool is_cooling;          /* Marks this as one of the profile's own trailing "Cooling" segment(s) - heater is forced off and the session phase auto-transitions to ROAST_PHASE_COOLING when the curve enters a segment like this (profile_curve_follower.c), instead of requiring a manual "Start Cooling" button. Cooling duration is therefore just this segment's (or segments') duration_s, configured per-profile like any other setpoint; target_temp_c/target_fan_pct are fixed (not operator-editable) at ROAST_PROFILE_COOLING_TEMP_C/ROAST_PROFILE_COOLING_FAN_PCT. */
} roast_profile_point_t;

typedef struct {
    char name[ROAST_PROFILE_NAME_MAX_LEN];
    uint8_t point_count;
    roast_profile_point_t points[ROAST_PROFILE_MAX_POINTS];
} roast_profile_t;

/** Sum of every setpoint's duration - the exact total length of the roast this profile describes. */
uint32_t roast_profile_total_duration_s(const roast_profile_t *profile);

/** Piecewise-linear-interpolated target BT at `elapsed_s` into the profile (clamped to the first/last setpoint's target outside the profile's range). */
float roast_profile_get_target_temp_c(const roast_profile_t *profile, uint32_t elapsed_s);

/** Piecewise-linear-interpolated target fan% at `elapsed_s` into the profile (clamped to the first/last setpoint's target outside the profile's range). */
uint8_t roast_profile_get_target_fan_pct(const roast_profile_t *profile, uint32_t elapsed_s);

/** Returns the index (0-based) of the setpoint segment `elapsed_s` falls into - used by the T034/T035 curve follower to detect when a manual override should expire (segment boundary crossed). Clamped to the last segment past the profile's total duration. */
uint8_t roast_profile_get_segment_index(const roast_profile_t *profile, uint32_t elapsed_s);

#ifdef __cplusplus
}
#endif

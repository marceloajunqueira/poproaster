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
 * Operator-reported problem (2026-08-09): holding BT flat for a whole
 * segment lets internal steam pressure vent/equalize instead of keep
 * building, which can suppress first crack - matches how real Ramp/Soak
 * roast controllers (and Artisan's own PID "RS" ramp/soak tables) work.
 * `roast_profile_get_target_temp_c()` therefore RAMPS interior segments: the
 * target rises linearly from the previous segment's own target to this
 * segment's target, over this segment's own duration (0% at the segment's
 * start, 100% at its end) - so a segment set to the SAME temp as the
 * previous one is still a flat hold by construction, and two different
 * values always produce a continuous rise/fall with no plateau.
 *
 * EXCEPTIONS, both per further operator feedback the same day - these two
 * segments are ALWAYS a flat STEP, never ramped:
 * - Segment 0 has no earlier point to ramp from, and PREHEAT already brings
 *   BT close to its target before Charge - ramping it anyway made Charge
 *   look like it "reset" the target back down and slowly re-climbed.
 * - The trailing Cooling segment: the heater is cut immediately the moment
 *   Cooling starts (session_sm_start_cooling()) - a ramping target curve
 *   suggested a gradual, planned cool-down that doesn't reflect reality.
 *
 * `roast_profile_get_target_fan_pct()` is unaffected - fan% always steps
 * instantly at every segment boundary.
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

/** Fan may never drop below this while the heater could be active - normal (non-Cooling) segments must keep target_fan_pct at/above this floor (enforced both by the profile editor UI and, independently, by the Safety Manager at the point commands are actually applied). Matches SAFETY_FAN_MIN_PCT_DURING_HEAT / Level 1 (hal/fan_pwm.h's fan physical minimum operating duty). Fan speed is quantized to 3 discrete levels (90/95/100%, see hal/fan_pwm.h) - this is Level 1's percentage, the lowest non-cooling segments may use. */
#define ROAST_PROFILE_FAN_MIN_PCT 90

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

/** Piecewise-linear-ramped target BT at `elapsed_s` into the profile (clamped to the first/last setpoint's target outside the profile's range) - segment 0 and the trailing Cooling segment are always a flat step, see the file doc comment. */
float roast_profile_get_target_temp_c(const roast_profile_t *profile, uint32_t elapsed_s);

/** Piecewise-linear-interpolated target fan% at `elapsed_s` into the profile (clamped to the first/last setpoint's target outside the profile's range). */
uint8_t roast_profile_get_target_fan_pct(const roast_profile_t *profile, uint32_t elapsed_s);

/** Returns the index (0-based) of the setpoint segment `elapsed_s` falls into - used by the T034/T035 curve follower to detect when a manual override should expire (segment boundary crossed). Clamped to the last segment past the profile's total duration. */
uint8_t roast_profile_get_segment_index(const roast_profile_t *profile, uint32_t elapsed_s);

/** Cumulative duration through and including `segment_idx` (i.e. that segment's own end time) - used by the operator "Next segment" skip. */
uint32_t roast_profile_get_segment_end_s(const roast_profile_t *profile, uint8_t segment_idx);

/**
 * Per operator requirement: every profile must end with exactly one
 * Cooling segment (heater forced off, fixed fan speed) - it's no longer an
 * optional per-segment toggle. Normalizes `profile` in place so its LAST
 * point is always is_cooling=true (appending a default-duration one if
 * needed, or converting the existing last segment if already at
 * ROAST_PROFILE_MAX_POINTS), and demotes any INTERMEDIATE point that was
 * (e.g. from older/imported data) marked is_cooling back to a normal
 * segment. Called by both the on-device editor (profile_editor.c) and the
 * web editor (presets_routes.c) whenever a profile is loaded/created/
 * imported, so the UI never needs to render a per-segment Cooling
 * toggle - the last segment simply always IS the Cooling one.
 */
void roast_profile_ensure_trailing_cooling(roast_profile_t *profile);

#ifdef __cplusplus
}
#endif

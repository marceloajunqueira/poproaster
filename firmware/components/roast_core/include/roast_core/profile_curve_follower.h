/**
 * @file profile_curve_follower.h
 * @brief T034/T035/T038: Profile-mode curve-following control loop.
 *
 * While the active session is in ROAST_MODE_PROFILE and the phase is
 * ROASTING/DEVELOPMENT (i.e. after the operator has confirmed CHARGE, see
 * session_state_machine.h's session_sm_confirm_charge()), this drives the
 * fan directly from the selected profile's per-segment target_fan_pct
 * (open-loop), and the heater via a closed-loop PID controller
 * (roast_core/heater_pid.h) tracking the segment's target_temp_c against
 * the live BT sensor reading - there is no operator-configurable heater
 * power setpoint.
 *
 * T035 (manual override): if a display/web command changes the actual
 * fan/heater away from whatever this module itself last wrote, that's
 * treated as an operator override - the curve follower backs off and
 * leaves the override in place until the profile's timeline crosses into
 * the NEXT setpoint segment, at which point automatic control resumes.
 *
 * T038 (auto Cooling): once elapsed roast time reaches the profile's total
 * duration (its final setpoint's segment end - the "drop point"), this
 * module automatically calls session_sm_start_cooling().
 *
 * Manual/Artisan mode (operator request): the heater is NEVER a direct
 * open-loop operator setpoint, even outside Profile mode - the operator
 * only ever picks a target bean temperature (see
 * profile_curve_follower_set_manual_target_temp_c() below, wired to the
 * Manual screen's "Target Temp" slider) and fan speed (still a plain
 * direct setpoint, command_dispatcher_set_fan_pct()); the SAME closed-loop
 * PID used for Profile mode drives the heater toward that target
 * automatically. If the operator wants heat but left the fan below the
 * fixed 60% (Level 1) floor, it's auto-raised to that floor instead of the heater
 * request just silently being rejected.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Manual mode "Target Temp" operating range - operator testing showed
 * setpoints above this melt the roaster's plastic housing, so this is the
 * hard ceiling for any manual target (see the clamp in
 * profile_curve_follower_set_manual_target_temp_c()), not just the
 * on-device slider's display range. 0 (heater off) is always still
 * reachable below this floor - it's a sentinel to turn the heater off
 * (Desligar button, or a stale/negative web request), not a real operating
 * temperature. */
#define MANUAL_TARGET_TEMP_MIN_C_INT 120
#define MANUAL_TARGET_TEMP_MAX_C_INT 220
#define MANUAL_TARGET_TEMP_MIN_C ((float)MANUAL_TARGET_TEMP_MIN_C_INT)
#define MANUAL_TARGET_TEMP_MAX_C ((float)MANUAL_TARGET_TEMP_MAX_C_INT)

#define PCF_STRINGIFY_(x) #x
#define PCF_STRINGIFY(x) PCF_STRINGIFY_(x)
/** Same bounds as plain string literals, for building HTML attributes (see dashboard_routes.c). */
#define MANUAL_TARGET_TEMP_MIN_C_STR PCF_STRINGIFY(MANUAL_TARGET_TEMP_MIN_C_INT)
#define MANUAL_TARGET_TEMP_MAX_C_STR PCF_STRINGIFY(MANUAL_TARGET_TEMP_MAX_C_INT)

/** Starts the background control-loop timer (1s period). Call once at boot, after command_dispatcher_init()/roast_telemetry_service_init(). */
esp_err_t profile_curve_follower_init(void);

/**
 * Manual/Artisan mode only: sets the target bean temperature the
 * closed-loop heater PID should automatically track - fan is left
 * entirely to the operator's own Fan slider/command_dispatcher_set_fan_pct().
 * Takes effect on the very next follower tick while a Manual-mode session
 * is in PREHEAT/ROASTING/DEVELOPMENT (never during COOLING - heater stays
 * forced off there regardless, same as Profile mode). Reset to 0.0f
 * (heater fully off) at the start of every new session, so a stale target
 * from a previous roast is never silently inherited.
 */
void profile_curve_follower_set_manual_target_temp_c(float target_c);

/** Returns whatever profile_curve_follower_set_manual_target_temp_c() last set - lets the Manual screen sync its Target Temp slider (e.g. after navigating away and back) without keeping its own separate copy of this state. */
float profile_curve_follower_get_manual_target_temp_c(void);

/**
 * PID tuning aid (operator-requested): open-loop step-response test. Bypasses
 * the PID entirely and commands the heater to a FIXED duty (0-100), logging
 * every follower tick to the same PID debug log (roast_core/pid_debug_log.h,
 * mode="STEPTEST") so the resulting bean-temp curve can be downloaded and
 * used to characterize the plant's real thermal lag/time constant - this
 * hardware heats via forced air through a resistive coil (fan always on,
 * needed for airflow into the roasting chamber itself), so the fan is left
 * entirely to the operator's own Fan control; only the heater is overridden
 * here. Takes effect on the very next follower tick, superseding Manual
 * mode's own target-temperature PID for as long as it's active. Pass -1 to
 * stop the test and return control to the normal Manual/Profile PID path
 * (also forces the heater fully off immediately). Safety Manager's usual
 * fan-floor/temperature-cutoff rules still apply underneath this - it is
 * NOT a bypass of safety_manager.c, only of the PID math.
 */
void profile_curve_follower_set_step_test_heater_pct(int pct);

/** Returns the currently active step-test duty (0-100), or -1 if the step test is not running - lets the web UI reflect actual state. */
int profile_curve_follower_get_step_test_heater_pct(void);

/**
 * Operator-initiated "Next segment" skip: pushes the profile curve's own
 * timeline forward to the start of the next segment (the real/wall-clock
 * timer is unaffected - see session_sm_skip_curve_time()). If the current
 * segment is the last heating one, this naturally lands on the profile's
 * own trailing Cooling segment, which the existing T038 auto-transition
 * picks up on the very next tick, same as the timeline naturally running
 * out. Only valid for a loaded Profile-mode session in ROASTING/DEVELOPMENT
 * - returns ESP_ERR_INVALID_STATE otherwise (no profile loaded, Manual/
 * Artisan mode, or wrong phase).
 */
esp_err_t profile_curve_follower_skip_to_next_segment(void);

#ifdef __cplusplus
}
#endif

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
 * fixed 30% floor, it's auto-raised to that floor instead of the heater
 * request just silently being rejected.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

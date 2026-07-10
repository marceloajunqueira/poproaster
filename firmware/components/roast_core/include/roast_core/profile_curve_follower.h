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
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the background control-loop timer (1s period). Call once at boot, after command_dispatcher_init()/roast_telemetry_service_init(). */
esp_err_t profile_curve_follower_init(void);

#ifdef __cplusplus
}
#endif

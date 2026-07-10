/**
 * @file heater_pid.h
 * @brief Closed-loop bean-temperature controller for Profile-mode roasting.
 *
 * Replaces the old open-loop "target_heater_pct" setpoint (removed from
 * roast_profile_point_t): operators only pick the target BEAN TEMPERATURE
 * per segment, and this PID controller works out the heater duty cycle
 * needed to track it, using the live BT sensor reading as feedback -
 * profile_curve_follower.c calls this once per follower tick while a
 * Profile-mode session is in ROASTING/DEVELOPMENT.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resets the controller's internal state (integral accumulator, last
 * measurement) - call whenever a new session starts being followed, so it
 * never inherits windup/derivative state from a previous roast.
 */
void heater_pid_reset(void);

/**
 * Computes the next heater duty cycle (0-100%) to drive `measured_temp_c`
 * toward `target_temp_c`, given `dt_s` seconds elapsed since the previous
 * call. Output is clamped to [0, 100] with anti-windup on the integral
 * term; the Safety Manager still has the final say (30% fan floor,
 * absolute cutoff, etc.) once this value reaches command_dispatcher.
 */
uint8_t heater_pid_update(float target_temp_c, float measured_temp_c, float dt_s);

#ifdef __cplusplus
}
#endif

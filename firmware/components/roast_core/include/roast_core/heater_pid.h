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

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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
 * term; the Safety Manager still has the final say (60% fan floor / Level 1,
 * absolute cutoff, etc.) once this value reaches command_dispatcher.
 */
uint8_t heater_pid_update(float target_temp_c, float measured_temp_c, float dt_s);

/** Snapshot of the internal PID terms from the MOST RECENT heater_pid_update()
 * call - for tuning/diagnostics only (see roast_core/pid_debug_log.h), not
 * used by the control loop itself. */
typedef struct {
    float error_c;      /* target_temp_c - measured_temp_c at that call. */
    float p_term;       /* Proportional contribution. */
    float i_term;       /* Integral contribution (PID_KI * integral). */
    float d_term;        /* Derivative contribution. */
    float raw_output;    /* Unclamped p+i+d sum before the [0,100] clamp. */
    bool hard_cutoff;    /* True if this call hit the hard overshoot cutoff (forced to 0). */
} heater_pid_debug_t;

/** Fills `out` with the internal term breakdown from the most recent heater_pid_update() call. */
void heater_pid_get_last_debug(heater_pid_debug_t *out);

/**
 * Remaps a LOGICAL heater duty (0-100%, e.g. straight from heater_pid_update())
 * onto the PHYSICAL SSR duty that compensates the element's nonlinear real-
 * world response (see duty_curve_deadzone_pct/duty_curve_gamma above).
 *
 * Deliberately only for CLOSED-LOOP callers (Profile-mode segments, Manual
 * Target Temp) - Step Test and Autotune must keep commanding the EXACT raw
 * duty the operator/algorithm specified, uncompensated, since both are
 * open-loop hardware characterization tools whose whole point is to
 * observe the real plant's response to a KNOWN duty (compensating there
 * would corrupt the very data needed to tune this compensation curve).
 */
uint8_t heater_pid_compensate_duty_pct(uint8_t logical_pct);

/** Live-tunable controller parameters (see heater_pid_set_tuning()). */
typedef struct {
    float kp;
    float ki;
    float kd;
    float hard_overshoot_margin_c; /* Degrees ABOVE target at which the heater is forced fully off. */
    /* Low-pass filter time constant (seconds) applied to the MEASUREMENT
     * used by the derivative term only. 0 disables filtering entirely (raw
     * derivative, the original behavior). This sensor has real thermal lag
     * plus its own EMA smoothing (roaster_hal/max6675.c) on top -
     * operator-reported: a large Kd reacting to that already-lagged,
     * still-noisy signal at only 1Hz can amplify small fluctuations into a
     * visible "sobe e desce" oscillation that has nothing to do with Kp/Ki.
     * Filtering the derivative signal itself (standard PID practice,
     * sometimes called the "N" filter) addresses that directly instead of
     * just detuning Kd. */
    float d_filter_tau_s;
    /* Max rate (degrees C per second) the INTERNAL setpoint is allowed to
     * move toward whatever target_temp_c heater_pid_update() is called
     * with. <=0 disables ramping (the target is used as-is, instantly -
     * the original behavior). Operator-reported: a big instant step
     * (Manual Target Temp, or PREHEAT holding a segment's target flat from
     * room temp) winds the integral against a huge error for a long
     * stretch, and by the time it's near target there's already too much
     * stored heat "in flight" for the D term to arrest in time - the
     * result overshoots into hard_overshoot_margin_c and crashes. Ramping
     * the setpoint keeps the tracking error small throughout the approach
     * so the integral never winds up far enough to cause that overshoot in
     * the first place - this fixes the actual cause (feeding the loop a
     * step its real thermal lag/mass can't track) rather than just
     * widening the cutoff margin, which would only delay the same crash. */
    float setpoint_ramp_c_per_s;
    /* Operator-reported (2026-08-07): the heating element's real thermal
     * response is NOT linear in commanded duty - below ~65% it barely
     * heats at all (losses to the airstream dominate), then each %
     * above that gets noticeably more aggressive, i.e. the closed loop
     * spends most of its 0-100% authority in a near-dead zone and only a
     * narrow top slice actually does useful, increasingly strong work.
     * This is why a profile/manual approach can look "slow for a long
     * time, then suddenly aggressive" even with correct PID gains and
     * setpoint ramping - the PLANT itself is nonlinear, not just the
     * control loop. These two fields let heater_pid_compensate_duty_pct()
     * remap the PID's LOGICAL 0-100% output onto the PHYSICAL SSR duty
     * that actually produces roughly proportional real heating power:
     * physical = deadzone_pct + (100-deadzone_pct) * (logical/100)^(1/gamma).
     * deadzone_pct=0 and gamma=1.0 together are the identity (no
     * compensation, physical==logical) - the original behavior. First-pass
     * values below are estimates from operator feedback, not a measured
     * curve; refine them using the web Diagnostics page's Step Test tool
     * (hold a few fixed duties and compare real BT rise) once real data is
     * available. */
    float duty_curve_deadzone_pct;
    float duty_curve_gamma;
} heater_pid_tuning_t;

/**
 * Reads the gains currently in effect. Values come from the compiled-in
 * defaults unless overridden at runtime (or restored from NVS at boot).
 */
void heater_pid_get_tuning(heater_pid_tuning_t *out);

/**
 * Operator-requested live tuning: replaces the active gains WITHOUT a
 * rebuild/reflash, so a tuning session can iterate in seconds instead of
 * one OTA cycle per attempt. Also resets the integral accumulator, since
 * an integral built up under different gains is meaningless afterwards.
 * Negative/zero-invalid values are rejected field by field (a field left
 * at a negative value keeps its current setting). `d_filter_tau_s` and
 * `setpoint_ramp_c_per_s` both use the same "negative = leave unchanged"
 * sentinel as kp/ki/kd (0 is a valid, meaningful value for either - "no
 * filtering" / "no ramping"). Set `persist` to store the new gains in NVS
 * so they survive a reboot.
 */
esp_err_t heater_pid_set_tuning(const heater_pid_tuning_t *tuning, bool persist);

/**
 * Restores gains previously saved via heater_pid_set_tuning(..., true).
 * Call once at boot (before the follower starts); silently keeps the
 * compiled-in defaults if nothing was ever persisted.
 */
esp_err_t heater_pid_load_tuning(void);

#ifdef __cplusplus
}
#endif

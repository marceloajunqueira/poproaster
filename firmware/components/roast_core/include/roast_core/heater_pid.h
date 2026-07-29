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
    bool protector_open; /* True if the element's thermal protector is believed open (see below). */
} heater_pid_debug_t;

/** Fills `out` with the internal term breakdown from the most recent heater_pid_update() call. */
void heater_pid_get_last_debug(heater_pid_debug_t *out);

/**
 * Thermal-protector observer state.
 *
 * This machine has TWO hardware thermal protectors (operator-confirmed): a
 * one-shot thermal fuse soldered to the element itself, and a resettable
 * bimetallic thermostat on the outside of the air tunnel. The bimetallic one
 * opens the heater circuit with NO electrical feedback to the firmware - the
 * only symptom is that the air temperature collapses while the SSR is still
 * being commanded on.
 *
 * Real log data (logs/pid_debug_4.csv) caught this twice in one session, both
 * times peaking at 194.2-194.3C before the collapse (0.1C repeatability - a
 * bimetal signature, not a control artifact), then falling ~48C in ~26s while
 * the heater was commanded at its maximum. Without detection the controller
 * responds exactly wrongly: it sees the temperature falling, winds the
 * integral to its 100% ceiling, and then slams full power in the instant the
 * bimetal re-closes, driving straight back into the trip. That is the
 * self-sustaining cycle seen in the log, and it hammers the protector.
 *
 * NOTE: this is an OBSERVER, not a replacement for the hardware protectors.
 * They remain the last line of defence and must never be bypassed; the point
 * of detecting them is to stop the controller from repeatedly provoking them.
 */
typedef struct {
    bool open;               /* Protector believed open RIGHT NOW. */
    uint32_t trip_count;     /* Trips detected since boot (0 = healthy). */
    float last_trip_temp_c;  /* Measured temperature at the onset of the last trip. */
    float ceiling_c;         /* Learned target ceiling, 0 if none learned yet. */
} heater_pid_protector_status_t;

/**
 * Reports the duty that was ACTUALLY applied to the SSR after the Safety
 * Manager's scaling/interlocks - call once per control tick, right after
 * applying the value returned by heater_pid_update().
 *
 * The observer needs this rather than the PID's own output because a
 * temperature fall is only suspicious if the heater was genuinely being
 * driven: with the Max Heater Power cap at 0%, or a safety interlock
 * rejecting commands, the PID can be asking for 100% while nothing at all
 * reaches the element - which is a perfectly normal reason to cool down.
 */
void heater_pid_note_applied_pct(uint8_t applied_pct);

/** Reads the current thermal-protector observer state. */
void heater_pid_get_protector_status(heater_pid_protector_status_t *out);

/**
 * Clears the observer's latched state, including any learned ceiling -
 * for the operator to call after physically investigating the machine.
 */
void heater_pid_clear_protector_state(void);

/** Live-tunable controller parameters (see heater_pid_set_tuning()). */
typedef struct {
    float kp;
    float ki;
    float kd;
    float hard_overshoot_margin_c; /* Degrees ABOVE target at which the heater is forced fully off. */
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
 * at a negative value keeps its current setting). Set `persist` to store
 * the new gains in NVS so they survive a reboot.
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

/**
 * @file pid_autotune.h
 * @brief Relay-feedback (Astrom-Hagglund) PID autotuner, Ziegler-Nichols rules.
 *
 * Port of the algorithm ESPHome uses in its `pid` climate component
 * (esphome/components/pid/pid_autotuner.cpp), adapted to this firmware's
 * 0-100% heater duty and to this machine's hardware constraints.
 *
 * How it works: a bang-bang relay with hysteresis drives the heater between
 * two duty levels, deliberately making the temperature oscillate around the
 * setpoint. The oscillation's period gives Pu; its amplitude `a` gives the
 * ultimate gain Ku = 4d/(pi*a), where d is half the relay's output span.
 * Ziegler-Nichols then converts (Ku, Pu) into gains.
 *
 * IMPORTANT - the result is only valid for the FAN LEVEL it was measured at.
 * On this roaster the airflow sets the process gain (measured: 64% duty at
 * fan 100% tops out near 140C, the same duty at fan 90% goes far higher), so
 * a tune done at one fan level does not transfer to another.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PID_AUTOTUNE_IDLE,
    PID_AUTOTUNE_RUNNING,
    PID_AUTOTUNE_SUCCEEDED,
    PID_AUTOTUNE_FAILED,
} pid_autotune_state_t;

typedef struct {
    float kp;
    float ki;
    float kd;
} pid_autotune_gains_t;

typedef struct {
    pid_autotune_state_t state;
    float setpoint_c;
    uint8_t output_positive_pct;
    uint8_t output_negative_pct;
    uint8_t fan_pct_at_start;   /* Gains are only valid for this airflow. */
    uint32_t elapsed_s;
    uint32_t phase_count;       /* Relay direction changes so far. */
    uint32_t zc_count;          /* Zero crossings recorded. */
    float ku;
    float pu_s;
    pid_autotune_gains_t zn_classic;     /* Ziegler-Nichols PID. */
    pid_autotune_gains_t some_overshoot;
    pid_autotune_gains_t no_overshoot;
    bool amplitude_convergent;  /* False => outside influence disturbed the run. */
    bool zc_symmetrical;        /* False => heating/cooling rates too different. */
    char message[96];
} pid_autotune_status_t;

/**
 * Starts a tuning run. `output_negative_pct` is normally 0 (this machine can
 * only heat); `output_positive_pct` should be a duty that reaches the
 * setpoint comfortably but not violently.
 *
 * Works standalone - no roast session needs to be started first. Rejected
 * if a run is already active, if the setpoint is above the thermal
 * protector's learned ceiling, or if the fan is below the safety floor -
 * the run must not be the thing that trips the protector.
 */
esp_err_t pid_autotune_start(float setpoint_c, uint8_t output_positive_pct, uint8_t output_negative_pct);

/** Stops a run and forces the heater off. Safe to call when not running. */
void pid_autotune_abort(const char *reason);

bool pid_autotune_is_active(void);

/** Feeds one measurement and returns the heater duty to apply this tick. */
uint8_t pid_autotune_update(float measured_c);

void pid_autotune_get_status(pid_autotune_status_t *out);

/** Applies one of the computed rule sets to the live PID. */
esp_err_t pid_autotune_apply_result(const pid_autotune_gains_t *gains, bool persist);

#ifdef __cplusplus
}
#endif

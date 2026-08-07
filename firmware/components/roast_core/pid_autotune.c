/**
 * @file pid_autotune.c
 * @brief Relay-feedback PID autotuner - see pid_autotune.h.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "hal/fan_pwm.h"
#include "roast_core/heater_pid.h"
#include "roast_core/pid_autotune.h"
#include "safety/safety_manager.h"

static const char *TAG = "pid_autotune";

#define AUTOTUNE_NOISEBAND_C 1.0f

/* General safety margin so a run can't climb indefinitely if something is
 * wrong - independent of any specific hardware protector. */
#define AUTOTUNE_OVERSHOOT_ABORT_C 25.0f
#define AUTOTUNE_TIMEOUT_S 2400
/* Operator-requested (2026-08-05): a real run (Pu=55.9s measured) took
 * ~22.5 minutes to finish because it never converged symmetrically and ran
 * all the way to the old cap of 40 phases. Halving it caps the same
 * worst case at roughly 20 phases * ~28s (half of Pu) =~ 9-10 minutes,
 * matching the requested ~10 min target - a well-converging (symmetric)
 * run still finishes far earlier than this on its own via
 * has_enough_data()+zc_symmetrical(), this only bounds the WORST case. */
#define AUTOTUNE_MAX_PHASES 20

/* ESPHome keeps 7 peak samples and needs 3 before it will compute. */
#define AUTOTUNE_PEAKS_MAX 7
#define AUTOTUNE_PEAKS_MIN 3
#define AUTOTUNE_ZC_MAX 16

typedef enum {
    RELAY_INIT,
    RELAY_POSITIVE,
    RELAY_NEGATIVE,
} relay_state_t;

static pid_autotune_state_t s_state = PID_AUTOTUNE_IDLE;
static float s_setpoint_c;
static uint8_t s_output_positive_pct;
static uint8_t s_output_negative_pct;
static uint8_t s_fan_pct_at_start;
static uint8_t s_cap_pct_at_start; /* Max Heater Power cap when the run started - see header note. */
static int64_t s_start_ms;
static int64_t s_end_ms; /* Frozen the instant the run stops - see get_status()'s elapsed_s. */
static char s_message[96];

static relay_state_t s_relay_state;
static uint32_t s_phase_count;

static relay_state_t s_freq_state;
static int64_t s_last_zc_ms;
static uint32_t s_zc_intervals_ms[AUTOTUNE_ZC_MAX];
static uint8_t s_zc_count;

static relay_state_t s_ampl_last_relay;
static float s_phase_min;
static float s_phase_max;
static float s_phase_mins[AUTOTUNE_PEAKS_MAX];
static float s_phase_maxs[AUTOTUNE_PEAKS_MAX];
static uint8_t s_phase_mins_n;
static uint8_t s_phase_maxs_n;

static float s_ku;
static float s_pu_s;

static void push_float(float *arr, uint8_t *n, float v)
{
    if (*n < AUTOTUNE_PEAKS_MAX) {
        arr[(*n)++] = v;
        return;
    }
    memmove(arr, arr + 1, (AUTOTUNE_PEAKS_MAX - 1) * sizeof(float));
    arr[AUTOTUNE_PEAKS_MAX - 1] = v;
}

static void reset_detectors(void)
{
    s_relay_state = RELAY_INIT;
    s_phase_count = 0;
    s_freq_state = RELAY_INIT;
    s_last_zc_ms = 0;
    s_zc_count = 0;
    s_ampl_last_relay = RELAY_INIT;
    s_phase_min = NAN;
    s_phase_max = NAN;
    s_phase_mins_n = 0;
    s_phase_maxs_n = 0;
    s_ku = 0.0f;
    s_pu_s = 0.0f;
}

static uint8_t relay_update(float error)
{
    if (s_relay_state == RELAY_INIT) {
        s_relay_state = (error > AUTOTUNE_NOISEBAND_C) ? RELAY_POSITIVE : RELAY_NEGATIVE;
    } else if (s_relay_state == RELAY_POSITIVE && error < -AUTOTUNE_NOISEBAND_C) {
        s_relay_state = RELAY_NEGATIVE;
        s_phase_count++;
    } else if (s_relay_state == RELAY_NEGATIVE && error > AUTOTUNE_NOISEBAND_C) {
        s_relay_state = RELAY_POSITIVE;
        s_phase_count++;
    }
    return (s_relay_state == RELAY_POSITIVE) ? s_output_positive_pct : s_output_negative_pct;
}

/* Crossings use a quarter of the relay's band so noise doesn't fake a period. */
static void frequency_update(int64_t now_ms, float error)
{
    const float band = AUTOTUNE_NOISEBAND_C / 4.0f;
    bool crossed = false;

    if (s_freq_state == RELAY_INIT) {
        s_freq_state = (error > band) ? RELAY_POSITIVE : RELAY_NEGATIVE;
    } else if (s_freq_state == RELAY_POSITIVE && error < -band) {
        s_freq_state = RELAY_NEGATIVE;
        crossed = true;
    } else if (s_freq_state == RELAY_NEGATIVE && error > band) {
        s_freq_state = RELAY_POSITIVE;
        crossed = true;
    }

    if (!crossed) {
        return;
    }
    if (s_last_zc_ms != 0) {
        uint32_t dt = (uint32_t)(now_ms - s_last_zc_ms);
        if (s_zc_count < AUTOTUNE_ZC_MAX) {
            s_zc_intervals_ms[s_zc_count++] = dt;
        } else {
            memmove(s_zc_intervals_ms, s_zc_intervals_ms + 1, (AUTOTUNE_ZC_MAX - 1) * sizeof(uint32_t));
            s_zc_intervals_ms[AUTOTUNE_ZC_MAX - 1] = dt;
        }
    }
    s_last_zc_ms = now_ms;
}

/* A peak always lands in the segment BEFORE the relay flips, so it is
 * recorded on the transition. */
static void amplitude_update(float error, relay_state_t relay_state)
{
    if (relay_state != s_ampl_last_relay) {
        if (s_ampl_last_relay == RELAY_POSITIVE) {
            push_float(s_phase_maxs, &s_phase_maxs_n, s_phase_max);
        } else if (s_ampl_last_relay == RELAY_NEGATIVE) {
            push_float(s_phase_mins, &s_phase_mins_n, s_phase_min);
        }
        s_phase_min = error;
        s_phase_max = error;
    }
    s_ampl_last_relay = relay_state;

    if (isnan(s_phase_min) || error < s_phase_min) {
        s_phase_min = error;
    }
    if (isnan(s_phase_max) || error > s_phase_max) {
        s_phase_max = error;
    }
}

static uint8_t peaks_available(void)
{
    return (s_phase_mins_n < s_phase_maxs_n) ? s_phase_mins_n : s_phase_maxs_n;
}

static float mean_oscillation_period_s(void)
{
    if (s_zc_count == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (uint8_t i = 0; i < s_zc_count; i++) {
        sum += (float)s_zc_intervals_ms[i];
    }
    /* Two crossings per full period. */
    return (sum / (float)s_zc_count) / 1000.0f * 2.0f;
}

static float mean_oscillation_amplitude(void)
{
    uint8_t n = peaks_available();
    if (n < 2) {
        return 0.0f;
    }
    float total = 0.0f;
    uint8_t count = 0;
    for (uint8_t i = 1; i + 1 < n; i++) {
        total += fabsf(s_phase_maxs[i] - s_phase_mins[i + 1]);
        count++;
    }
    if (count == 0) {
        return 0.0f;
    }
    return (total / (float)count) / 2.0f;
}

static bool zc_symmetrical(void)
{
    if (s_zc_count == 0) {
        return false;
    }
    uint32_t max_i = s_zc_intervals_ms[0];
    uint32_t min_i = s_zc_intervals_ms[0];
    for (uint8_t i = 0; i < s_zc_count; i++) {
        if (s_zc_intervals_ms[i] > max_i) {
            max_i = s_zc_intervals_ms[i];
        }
        if (s_zc_intervals_ms[i] < min_i) {
            min_i = s_zc_intervals_ms[i];
        }
    }
    return max_i > 0 && ((float)min_i / (float)max_i) >= 0.66f;
}

static bool amplitude_convergent(void)
{
    if (s_phase_mins_n == 0 || s_phase_maxs_n == 0) {
        return false;
    }
    float global_max = s_phase_maxs[0];
    float global_min = s_phase_mins[0];
    for (uint8_t i = 0; i < s_phase_maxs_n; i++) {
        if (s_phase_maxs[i] > global_max) {
            global_max = s_phase_maxs[i];
        }
    }
    for (uint8_t i = 0; i < s_phase_mins_n; i++) {
        if (s_phase_mins[i] < global_min) {
            global_min = s_phase_mins[i];
        }
    }
    float global_amplitude = (global_max - global_min) / 2.0f;
    if (global_amplitude <= 0.0f) {
        return false;
    }
    /* ESPHome omits the absolute value here, which makes the check pass even
     * for wildly scattered amplitudes; compare magnitudes instead. */
    return fabsf(mean_oscillation_amplitude() - global_amplitude) / global_amplitude < 0.05f;
}

static bool has_enough_data(void)
{
    return s_zc_count >= 2 && peaks_available() >= AUTOTUNE_PEAKS_MIN;
}

static pid_autotune_gains_t rule(float kp_f, float ki_f, float kd_f)
{
    pid_autotune_gains_t g = {
        .kp = kp_f * s_ku,
        .ki = (s_pu_s > 0.0f) ? (ki_f * s_ku / s_pu_s) : 0.0f,
        .kd = kd_f * s_ku * s_pu_s,
    };
    return g;
}

static void finish_success(void)
{
    s_end_ms = esp_timer_get_time() / 1000;
    float amplitude = mean_oscillation_amplitude();
    if (amplitude <= 0.0f) {
        s_state = PID_AUTOTUNE_FAILED;
        snprintf(s_message, sizeof(s_message), "No usable oscillation amplitude");
        return;
    }
    /* Use the REAL delivered duty (after the Max Heater Power cap), not the
     * nominal relay request - otherwise Ku is computed for a relay swing
     * that never actually reached the heater whenever a cap is active. */
    float eff_positive = (float)s_output_positive_pct * (float)s_cap_pct_at_start / 100.0f;
    float eff_negative = (float)s_output_negative_pct * (float)s_cap_pct_at_start / 100.0f;
    float d = (eff_positive - eff_negative) / 2.0f;
    s_ku = 4.0f * d / ((float)M_PI * amplitude);
    s_pu_s = mean_oscillation_period_s();
    s_state = PID_AUTOTUNE_SUCCEEDED;

    pid_autotune_gains_t zn = rule(0.6f, 1.2f, 0.075f);
    snprintf(s_message, sizeof(s_message), "Ku=%.2f Pu=%.1fs -> kp=%.3f ki=%.4f kd=%.2f", (double)s_ku,
             (double)s_pu_s, (double)zn.kp, (double)zn.ki, (double)zn.kd);
    ESP_LOGI(TAG, "Autotune finished: %s (fan %u%%)", s_message, (unsigned)s_fan_pct_at_start);
    if (!zc_symmetrical()) {
        ESP_LOGW(TAG, "Heating and cooling rates differ a lot - try a lower positive output");
    }
    if (!amplitude_convergent()) {
        ESP_LOGW(TAG, "Oscillation amplitude did not converge - something disturbed the run");
    }
}

esp_err_t pid_autotune_start(float setpoint_c, uint8_t output_positive_pct, uint8_t output_negative_pct)
{
    if (s_state == PID_AUTOTUNE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (setpoint_c <= 0.0f || output_positive_pct > 100 || output_negative_pct >= output_positive_pct) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fan_pwm_get_pct() < SAFETY_FAN_MIN_PCT_DURING_HEAT) {
        ESP_LOGW(TAG, "Refusing to start: fan below the %u%% heating floor",
                 (unsigned)SAFETY_FAN_MIN_PCT_DURING_HEAT);
        return ESP_ERR_INVALID_STATE;
    }

    s_setpoint_c = setpoint_c;
    s_output_positive_pct = output_positive_pct;
    s_output_negative_pct = output_negative_pct;
    s_fan_pct_at_start = fan_pwm_get_pct();
    s_cap_pct_at_start = safety_manager_get_max_heater_power_pct();
    s_start_ms = esp_timer_get_time() / 1000;
    s_state = PID_AUTOTUNE_RUNNING;
    snprintf(s_message, sizeof(s_message), "Running");
    reset_detectors();

    ESP_LOGI(TAG, "Autotune started: setpoint %.1f C, relay %u%%/%u%%, fan %u%%, heater cap %u%%",
             (double)setpoint_c, (unsigned)output_positive_pct, (unsigned)output_negative_pct,
             (unsigned)s_fan_pct_at_start, (unsigned)s_cap_pct_at_start);
    if (s_cap_pct_at_start < 100) {
        uint32_t eff_pct = ((uint32_t)output_positive_pct * s_cap_pct_at_start) / 100;
        ESP_LOGW(TAG,
                 "Max Heater Power cap is %u%% - the requested %u%% relay output will actually deliver only "
                 "%u%% to the heater. The run will still be correct but slower; consider raising the cap to "
                 "100%% for a faster/cleaner autotune.",
                 (unsigned)s_cap_pct_at_start, (unsigned)output_positive_pct, (unsigned)eff_pct);
    }
    return ESP_OK;
}

void pid_autotune_abort(const char *reason)
{
    if (s_state != PID_AUTOTUNE_RUNNING) {
        return;
    }
    s_end_ms = esp_timer_get_time() / 1000;
    s_state = PID_AUTOTUNE_FAILED;
    snprintf(s_message, sizeof(s_message), "%s", (reason != NULL) ? reason : "Aborted");
    ESP_LOGW(TAG, "Autotune aborted: %s", s_message);
}

bool pid_autotune_is_active(void)
{
    return s_state == PID_AUTOTUNE_RUNNING;
}

uint8_t pid_autotune_update(float measured_c)
{
    if (s_state != PID_AUTOTUNE_RUNNING) {
        return 0;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t elapsed_s = (uint32_t)((now_ms - s_start_ms) / 1000);

    if (measured_c > s_setpoint_c + AUTOTUNE_OVERSHOOT_ABORT_C) {
        pid_autotune_abort("Overshot the setpoint too far");
        return 0;
    }
    if (elapsed_s > AUTOTUNE_TIMEOUT_S) {
        pid_autotune_abort("Timed out without a usable oscillation");
        return 0;
    }
    if (fan_pwm_get_pct() < SAFETY_FAN_MIN_PCT_DURING_HEAT) {
        pid_autotune_abort("Fan dropped below the heating floor");
        return 0;
    }

    float error = s_setpoint_c - measured_c;
    uint8_t output = relay_update(error);
    frequency_update(now_ms, error);
    amplitude_update(error, s_relay_state);

    /* Live progress feedback (overwritten below by finish_success()/abort()
     * if this same tick concludes the run) - tells the operator WHY the
     * heater reading might look lower than the configured relay output, and
     * what the run is currently doing, instead of a static "Running" for
     * the whole duration. */
    const char *dir = (s_relay_state == RELAY_POSITIVE) ? "Heating" : "Cooling";
    if (s_cap_pct_at_start < 100) {
        uint32_t eff_pct = ((uint32_t)s_output_positive_pct * s_cap_pct_at_start) / 100;
        snprintf(s_message, sizeof(s_message), "%s (BT=%.1fC, target %.1fC) - heater capped %u%%->%u%%", dir,
                 (double)measured_c, (double)s_setpoint_c, (unsigned)s_output_positive_pct, (unsigned)eff_pct);
    } else {
        snprintf(s_message, sizeof(s_message), "%s (BT=%.1fC, target %.1fC)", dir, (double)measured_c,
                 (double)s_setpoint_c);
    }

    if (has_enough_data() && (zc_symmetrical() || s_phase_count >= AUTOTUNE_MAX_PHASES)) {
        finish_success();
        return 0;
    }
    if (s_phase_count >= AUTOTUNE_MAX_PHASES) {
        pid_autotune_abort("Too many relay phases without convergence");
        return 0;
    }

    return output;
}

void pid_autotune_get_status(pid_autotune_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->setpoint_c = s_setpoint_c;
    out->output_positive_pct = s_output_positive_pct;
    out->output_negative_pct = s_output_negative_pct;
    out->fan_pct_at_start = s_fan_pct_at_start;
    out->heater_power_cap_pct = s_cap_pct_at_start;
    out->effective_output_positive_pct = (uint8_t)(((uint32_t)s_output_positive_pct * s_cap_pct_at_start) / 100);
    out->relay_positive = (s_relay_state == RELAY_POSITIVE);
    out->phase_count_max = AUTOTUNE_MAX_PHASES;
    /* BUG FIX (operator-reported): elapsed_s used to keep counting against
     * live time even after the run stopped (SUCCEEDED/FAILED), since it was
     * always measured against the current clock - freeze it against
     * s_end_ms once running has actually concluded, same fix shape as
     * session_state_machine.c's s_ended_at_ms for the roast dashboard timer. */
    if (s_state == PID_AUTOTUNE_IDLE) {
        out->elapsed_s = 0;
    } else if (s_state == PID_AUTOTUNE_RUNNING) {
        out->elapsed_s = (uint32_t)(((esp_timer_get_time() / 1000) - s_start_ms) / 1000);
    } else {
        out->elapsed_s = (uint32_t)((s_end_ms - s_start_ms) / 1000);
    }
    out->phase_count = s_phase_count;
    out->zc_count = s_zc_count;
    out->ku = s_ku;
    out->pu_s = s_pu_s;
    out->amplitude_convergent = amplitude_convergent();
    out->zc_symmetrical = zc_symmetrical();
    snprintf(out->message, sizeof(out->message), "%s", s_message);

    if (s_state == PID_AUTOTUNE_SUCCEEDED) {
        out->zn_classic = rule(0.6f, 1.2f, 0.075f);
        out->some_overshoot = rule(0.333f, 0.667f, 0.111f);
        out->no_overshoot = rule(0.2f, 0.4f, 0.0625f);
    }
}

esp_err_t pid_autotune_apply_result(const pid_autotune_gains_t *gains, bool persist)
{
    if (gains == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    heater_pid_tuning_t tuning = {
        .kp = gains->kp,
        .ki = gains->ki,
        .kd = gains->kd,
        .hard_overshoot_margin_c = -1.0f,
        .d_filter_tau_s = -1.0f, /* Autotune doesn't measure this - leave whatever is currently set. */
        .setpoint_ramp_c_per_s = -1.0f, /* Same - leave whatever is currently set. */
        .duty_curve_deadzone_pct = -1.0f, /* Same - autotune doesn't measure the duty curve either. */
        .duty_curve_gamma = -1.0f,
    };
    return heater_pid_set_tuning(&tuning, persist);
}

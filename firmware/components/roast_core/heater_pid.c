/**
 * @file heater_pid.c
 * @brief See header.
 */
#include <stdbool.h>
#include <math.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "roast_core/heater_pid.h"

static const char *TAG = "heater_pid";

/* Gains derived from a real open-loop step-response test on the actual
 * hardware (pid_debug_3.csv: 50% commanded / 40% applied duty, fan fixed at
 * 90%, forced-air coil heater with the BT probe in the air stream). Fitting
 * a first-order-plus-dead-time model to the measured rise (15.7C -> ~108.5C
 * steady state) via the three-point method gave: process gain K ~= 1.86
 * degC per 1% commanded duty, time constant tau ~= 45s, dead time L ~= 7s
 * (short, since this is forced air directly on the sensor, not a large
 * drum mass). IMC/lambda tuning (lambda ~= 9s, i.e. max(L, 0.2*tau)) from
 * those numbers gives Kc ~= 1.5, Ti ~= tau ~= 45s -> Ki ~= Kc/Ti, Td ~= L/2
 * -> Kd ~= Kc*Td. The PREVIOUS gains (KP=4.0) were ~8x more aggressive than
 * this model recommends - real log data (pid_debug_2.csv) showed p_term
 * ALONE saturating the output for any error above ~25C, which drove the
 * controller to run essentially bang-bang over a huge range and produced a
 * large, sustained oscillation (~20-40C swings) around the target instead
 * of a smooth approach. Derivative acts on the MEASURED temperature (not on
 * the error) to avoid a derivative "kick" every time the profile curve's
 * target jumps at a segment boundary. Retune here only, nothing else
 * depends on these values - but if retuning again, get a fresh step-test
 * log first (Diagnostics page) rather than guessing.
 *
 * These are DEFAULTS only - all four are live-tunable at runtime via
 * heater_pid_set_tuning() (web Diagnostics page / REST API), so a tuning
 * session can iterate without a rebuild+OTA per attempt. */
#define PID_KP_DEFAULT 1.5f
#define PID_KI_DEFAULT 0.034f
#define PID_KD_DEFAULT 5.3f
#define PID_OUTPUT_MIN 0.0f
#define PID_OUTPUT_MAX 100.0f

/* Operator-reported (2026-08-05): after an autotune run applied a large Kd
 * (~15), bean temp still "sobe e desce bastante" (oscillates a lot). The
 * BT sensor has real physical thermal lag (it sits in the air stream, not
 * on the element) PLUS its own EMA smoothing (roaster_hal/max6675.c) on
 * top - a derivative term computed every 1s from that already-lagged,
 * still-slightly-noisy signal amplifies small fluctuations into visible
 * output swings, independent of whether Kp/Ki themselves are reasonable.
 * This time constant low-pass-filters the MEASUREMENT fed to the D term
 * only. Default chosen as a moderate value relative to this plant's
 * measured ~56s oscillation period (pid_autotune.c) - enough to
 * meaningfully cut per-tick noise sensitivity without meaningfully lagging
 * the real trend. */
#define PID_D_FILTER_TAU_DEFAULT_S 2.0f

/* Operator-reported (2026-08-07): a big instant target step (Manual Target
 * Temp, or PREHEAT) reliably overshoots into the hard cutoff below on the
 * FIRST approach, then crashes and recovers. Root cause is the control loop
 * being fed a step its real thermal lag/mass can't track cleanly - see the
 * long comment on setpoint ramping in heater_pid_update(). 1.0C/s means a
 * 130C rise (e.g. room temp to a 175C target) ramps over ~130s (~2min) -
 * fast enough that PREHEAT isn't slowed to a crawl, but gentle enough that
 * the tracking error stays small throughout instead of spiking into a huge
 * integral windup. */
#define PID_SETPOINT_RAMP_DEFAULT_C_PER_S 1.0f

/* Operator-reported (2026-08-07), from hands-on testing: below ~65%
 * commanded duty the element barely heats at all, then each % above that
 * gets progressively more aggressive - real physical nonlinearity, not a
 * control-loop artifact. First-pass estimate only; refine via the web
 * Diagnostics page's Step Test tool (a few fixed-duty holds, compare real
 * BT rise) once real data is available - see heater_pid_compensate_duty_pct(). */
#define DUTY_CURVE_DEADZONE_DEFAULT_PCT 65.0f
#define DUTY_CURVE_GAMMA_DEFAULT 2.0f

#define PID_NVS_NAMESPACE "roast_cfg"
#define PID_NVS_KEY_KP "pid_kp"
#define PID_NVS_KEY_KI "pid_ki"
#define PID_NVS_KEY_KD "pid_kd"
#define PID_NVS_KEY_MARGIN "pid_margin"
#define PID_NVS_KEY_D_TAU "pid_dtau"
#define PID_NVS_KEY_SP_RAMP "pid_spramp"
#define PID_NVS_KEY_DUTY_DZ "pid_dutydz"
#define PID_NVS_KEY_DUTY_GAMMA "pid_dutygam"

/* SAFETY BACKSTOP: this drum's BT sensor sits in the circulating air (not
 * on the element itself), so there's a real, significant thermal lag
 * between the heater actually running and the sensor showing it -
 * operator-reported. Independent of the anti-windup fix below, if the
 * measured temperature is already this many degrees ABOVE target, force
 * the heater fully off immediately, no PID math involved - a hard,
 * unconditional ceiling. This is a LAST-RESORT backstop, not a routine
 * control mechanism - real test data (pid_debug.csv) showed a 3.0f margin
 * fires on essentially every approach to target (normal thermal coasting
 * alone carries measured_c 3-4C past target after cutoff fires), turning
 * this into a self-sustained ~20C amplitude / ~40s period limit-cycle
 * oscillation that cuts the heater fully off, blows cold air into the
 * drum, and drops far below target before recovering. Widened so the PID's
 * own (now-fixed) integral clamping gets a real chance to converge
 * smoothly before this backstop ever has to intervene. */
#define PID_HARD_OVERSHOOT_MARGIN_DEFAULT_C 8.0f

static float s_integral;
static float s_prev_measured_c;
static bool s_has_prev;
static float s_d_filtered; /* Low-pass-filtered rate of change, used ONLY for the D term - see PID_D_FILTER_TAU_DEFAULT_S. */
static float s_ramped_target_c; /* Internal setpoint, advanced toward the real target at a bounded rate - see PID_SETPOINT_RAMP_DEFAULT_C_PER_S. */
static bool s_ramp_has_value;
static heater_pid_debug_t s_last_debug;

static heater_pid_tuning_t s_tuning = {
    .kp = PID_KP_DEFAULT,
    .ki = PID_KI_DEFAULT,
    .kd = PID_KD_DEFAULT,
    .hard_overshoot_margin_c = PID_HARD_OVERSHOOT_MARGIN_DEFAULT_C,
    .d_filter_tau_s = PID_D_FILTER_TAU_DEFAULT_S,
    .setpoint_ramp_c_per_s = PID_SETPOINT_RAMP_DEFAULT_C_PER_S,
    .duty_curve_deadzone_pct = DUTY_CURVE_DEADZONE_DEFAULT_PCT,
    .duty_curve_gamma = DUTY_CURVE_GAMMA_DEFAULT,
};

void heater_pid_reset(void)
{
    s_integral = 0.0f;
    s_prev_measured_c = 0.0f;
    s_has_prev = false;
    s_d_filtered = 0.0f; /* Re-seeded from the first real rate sample below, like s_prev_measured_c. */
    s_ramp_has_value = false; /* Re-seeded from the current measurement on the next update() call. */
}

uint8_t heater_pid_update(float target_temp_c, float measured_temp_c, float dt_s)
{
    if (dt_s <= 0.0f) {
        dt_s = 1.0f;
    }

    /* Rate of change is needed by the derivative term, computed once up
     * front - before the hard overshoot cutoff, which returns early. */
    float d_measured = s_has_prev ? (measured_temp_c - s_prev_measured_c) / dt_s : 0.0f;
    bool had_prev = s_has_prev;
    s_prev_measured_c = measured_temp_c;
    s_has_prev = true;

    /* First-order low-pass filter of the rate, used ONLY by the D term
     * below. tau_s=0 makes alpha=1, i.e. the filtered value equals the raw
     * one instantly (no filtering) - operators who explicitly set the
     * tuning field to 0 get the exact old unfiltered behavior back. */
    float alpha_d = dt_s / (s_tuning.d_filter_tau_s + dt_s);
    s_d_filtered = had_prev ? (s_d_filtered + alpha_d * (d_measured - s_d_filtered)) : d_measured;

    /* Setpoint ramp (operator-reported: a big instant step - Manual Target
     * Temp, or PREHEAT holding a segment's target flat from room temp -
     * winds the integral hard against a huge error for a long stretch, and
     * by the time it's near target there's already too much stored heat
     * "in flight" for the D term to stop in time, overshooting into the
     * hard cutoff below and crashing. Raising the cutoff margin would only
     * delay that same crash, not fix it - the actual bug is feeding the
     * control loop a STEP when the real plant has real lag/mass. The fix:
     * never feed the P/I/D math a bigger jump than the plant can plausibly
     * track, by advancing an internal ramped setpoint toward the real
     * target at a bounded rate instead of jumping to it instantly. This
     * keeps the tracking error small throughout the approach, so the
     * integral never winds up far enough to cause a real overshoot in the
     * first place - the difference between preventing the problem and
     * just moving the cutoff further away. <=0 disables ramping (instant
     * target, the original behavior). The hard overshoot cutoff below
     * deliberately still compares against the REAL final target_temp_c,
     * not the ramped one, so it isn't weakened - it just rarely needs to
     * fire anymore. */
    float final_target_c = target_temp_c;
    if (s_tuning.setpoint_ramp_c_per_s <= 0.0f) {
        s_ramped_target_c = target_temp_c;
    } else {
        if (!s_ramp_has_value) {
            s_ramped_target_c = measured_temp_c;
            s_ramp_has_value = true;
        }
        float max_step = s_tuning.setpoint_ramp_c_per_s * dt_s;
        if (target_temp_c > s_ramped_target_c + max_step) {
            s_ramped_target_c += max_step;
        } else if (target_temp_c < s_ramped_target_c - max_step) {
            s_ramped_target_c -= max_step;
        } else {
            s_ramped_target_c = target_temp_c;
        }
    }
    target_temp_c = s_ramped_target_c;

    float error = target_temp_c - measured_temp_c;

    if (final_target_c - measured_temp_c <= -s_tuning.hard_overshoot_margin_c) {
        /* Already meaningfully over target - cut immediately and reset the
         * integral so there's no leftover windup once temperature comes
         * back down toward target. */
        s_integral = 0.0f;
        s_last_debug.error_c = final_target_c - measured_temp_c;
        s_last_debug.p_term = 0.0f;
        s_last_debug.i_term = 0.0f;
        s_last_debug.d_term = 0.0f;
        s_last_debug.raw_output = 0.0f;
        s_last_debug.hard_cutoff = true;
        return 0;
    }

    float p_term = s_tuning.kp * error;
    float d_term = -s_tuning.kd * s_d_filtered;

    /* Integral clamping anti-windup: bound the INTEGRAL'S OWN contribution
     * to the full output range, independently of p_term/d_term.
     *
     * The previous approach here ("back-calculation") solved for whatever
     * integral value would make p_term + i_term + d_term EXACTLY equal the
     * clamped output. That is wrong whenever p_term ALONE already exceeds
     * the output range - which is the NORMAL case at the start of every
     * preheat (room temp vs a 150-200C target gives error ~150-200C,
     * p_term = KP*error ~ 600-800, vastly more than the 100% ceiling).
     * Confirmed from real hardware log data (pid_debug.csv): on literally
     * the FIRST PID tick of a cold start, error=170.57C, p_term=682.29 -
     * the back-calculation formula computed s_integral = -11654 to force
     * the (impossible) exact match, poisoning the integral hugely negative
     * from tick one. As p_term naturally shrank while temperature climbed,
     * that poisoned integral increasingly dragged output down, collapsing
     * heater duty to ~32% while still only at 87.5C against a 194C target
     * - well before anything resembling "close to target".
     *
     * Clamping i_term directly avoids that: the integral can still grow
     * during a long sustained climb (helping close steady-state error),
     * but it can never be forced to a value that only makes sense in
     * combination with an oversized p_term it has nothing to do with. */
    float integral_candidate = s_integral + error * dt_s;
    float i_term = s_tuning.ki * integral_candidate;
    if (i_term > PID_OUTPUT_MAX) {
        i_term = PID_OUTPUT_MAX;
        integral_candidate = (s_tuning.ki > 0.0f) ? (i_term / s_tuning.ki) : integral_candidate;
    } else if (i_term < PID_OUTPUT_MIN) {
        i_term = PID_OUTPUT_MIN;
        integral_candidate = (s_tuning.ki > 0.0f) ? (i_term / s_tuning.ki) : integral_candidate;
    }
    s_integral = integral_candidate;

    float unclamped_output = p_term + i_term + d_term;

    float output = unclamped_output;
    if (output > PID_OUTPUT_MAX) {
        output = PID_OUTPUT_MAX;
    }
    if (output < PID_OUTPUT_MIN) {
        output = PID_OUTPUT_MIN;
    }

    s_last_debug.error_c = error;
    s_last_debug.p_term = p_term;
    s_last_debug.i_term = i_term;
    s_last_debug.d_term = d_term;
    s_last_debug.raw_output = unclamped_output;
    s_last_debug.hard_cutoff = false;

    return (uint8_t)(output + 0.5f);
}

void heater_pid_get_last_debug(heater_pid_debug_t *out)
{
    if (out != NULL) {
        *out = s_last_debug;
    }
}

uint8_t heater_pid_compensate_duty_pct(uint8_t logical_pct)
{
    if (logical_pct == 0) {
        return 0;
    }
    float dz = s_tuning.duty_curve_deadzone_pct;
    if (dz < 0.0f) {
        dz = 0.0f;
    } else if (dz > 99.0f) {
        dz = 99.0f;
    }
    float gamma = s_tuning.duty_curve_gamma;
    if (gamma < 0.1f) {
        gamma = 0.1f; /* Avoid a division blow-up; effectively "disabled" territory anyway. */
    }
    float frac = (float)logical_pct / 100.0f;
    float physical = dz + (100.0f - dz) * powf(frac, 1.0f / gamma);
    if (physical > 100.0f) {
        physical = 100.0f;
    } else if (physical < 0.0f) {
        physical = 0.0f;
    }
    return (uint8_t)(physical + 0.5f);
}

void heater_pid_get_tuning(heater_pid_tuning_t *out)
{
    if (out != NULL) {
        *out = s_tuning;
    }
}

esp_err_t heater_pid_set_tuning(const heater_pid_tuning_t *tuning, bool persist)
{
    if (tuning == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Each field is applied independently; a negative value means "leave this
     * one alone", which lets the API/UI change just one gain at a time. */
    if (tuning->kp >= 0.0f) {
        s_tuning.kp = tuning->kp;
    }
    if (tuning->ki >= 0.0f) {
        s_tuning.ki = tuning->ki;
    }
    if (tuning->kd >= 0.0f) {
        s_tuning.kd = tuning->kd;
    }
    if (tuning->hard_overshoot_margin_c > 0.0f) {
        s_tuning.hard_overshoot_margin_c = tuning->hard_overshoot_margin_c;
    }
    if (tuning->d_filter_tau_s >= 0.0f) {
        s_tuning.d_filter_tau_s = tuning->d_filter_tau_s;
    }
    if (tuning->setpoint_ramp_c_per_s >= 0.0f) {
        s_tuning.setpoint_ramp_c_per_s = tuning->setpoint_ramp_c_per_s;
    }
    if (tuning->duty_curve_deadzone_pct >= 0.0f) {
        s_tuning.duty_curve_deadzone_pct = tuning->duty_curve_deadzone_pct;
    }
    if (tuning->duty_curve_gamma >= 0.0f) {
        s_tuning.duty_curve_gamma = tuning->duty_curve_gamma;
    }

    /* An integral accumulated under the old gains has no meaning under the
     * new ones - carrying it over would produce a spurious output step right
     * after the change. */
    heater_pid_reset();

    ESP_LOGI(TAG,
             "PID tuning set: Kp=%.4f Ki=%.4f Kd=%.4f margin=%.1fC d_tau=%.1fs sp_ramp=%.2fC/s duty_dz=%.1f%% "
             "duty_gamma=%.2f%s",
             (double)s_tuning.kp, (double)s_tuning.ki, (double)s_tuning.kd,
             (double)s_tuning.hard_overshoot_margin_c, (double)s_tuning.d_filter_tau_s,
             (double)s_tuning.setpoint_ramp_c_per_s, (double)s_tuning.duty_curve_deadzone_pct,
             (double)s_tuning.duty_curve_gamma, persist ? " (persisted)" : "");

    if (!persist) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PID_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Stored as raw blobs since NVS has no native float type. */
    nvs_set_blob(nvs, PID_NVS_KEY_KP, &s_tuning.kp, sizeof(s_tuning.kp));
    nvs_set_blob(nvs, PID_NVS_KEY_KI, &s_tuning.ki, sizeof(s_tuning.ki));
    nvs_set_blob(nvs, PID_NVS_KEY_KD, &s_tuning.kd, sizeof(s_tuning.kd));
    nvs_set_blob(nvs, PID_NVS_KEY_MARGIN, &s_tuning.hard_overshoot_margin_c,
                 sizeof(s_tuning.hard_overshoot_margin_c));
    nvs_set_blob(nvs, PID_NVS_KEY_D_TAU, &s_tuning.d_filter_tau_s, sizeof(s_tuning.d_filter_tau_s));
    nvs_set_blob(nvs, PID_NVS_KEY_SP_RAMP, &s_tuning.setpoint_ramp_c_per_s, sizeof(s_tuning.setpoint_ramp_c_per_s));
    nvs_set_blob(nvs, PID_NVS_KEY_DUTY_DZ, &s_tuning.duty_curve_deadzone_pct, sizeof(s_tuning.duty_curve_deadzone_pct));
    nvs_set_blob(nvs, PID_NVS_KEY_DUTY_GAMMA, &s_tuning.duty_curve_gamma, sizeof(s_tuning.duty_curve_gamma));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/** Loads one persisted float, leaving `*value` untouched if it was never stored. */
static void load_float(nvs_handle_t nvs, const char *key, float *value)
{
    size_t len = sizeof(float);
    float stored = 0.0f;
    if (nvs_get_blob(nvs, key, &stored, &len) == ESP_OK && len == sizeof(float)) {
        *value = stored;
    }
}

esp_err_t heater_pid_load_tuning(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PID_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        /* Nothing persisted yet - compiled-in defaults stay in effect. */
        return ESP_OK;
    }
    load_float(nvs, PID_NVS_KEY_KP, &s_tuning.kp);
    load_float(nvs, PID_NVS_KEY_KI, &s_tuning.ki);
    load_float(nvs, PID_NVS_KEY_KD, &s_tuning.kd);
    load_float(nvs, PID_NVS_KEY_MARGIN, &s_tuning.hard_overshoot_margin_c);
    load_float(nvs, PID_NVS_KEY_D_TAU, &s_tuning.d_filter_tau_s);
    load_float(nvs, PID_NVS_KEY_SP_RAMP, &s_tuning.setpoint_ramp_c_per_s);
    load_float(nvs, PID_NVS_KEY_DUTY_DZ, &s_tuning.duty_curve_deadzone_pct);
    load_float(nvs, PID_NVS_KEY_DUTY_GAMMA, &s_tuning.duty_curve_gamma);
    nvs_close(nvs);

    ESP_LOGI(TAG,
             "PID tuning loaded: Kp=%.4f Ki=%.4f Kd=%.4f margin=%.1fC d_tau=%.1fs sp_ramp=%.2fC/s duty_dz=%.1f%% "
             "duty_gamma=%.2f",
             (double)s_tuning.kp, (double)s_tuning.ki, (double)s_tuning.kd,
             (double)s_tuning.hard_overshoot_margin_c, (double)s_tuning.d_filter_tau_s,
             (double)s_tuning.setpoint_ramp_c_per_s, (double)s_tuning.duty_curve_deadzone_pct,
             (double)s_tuning.duty_curve_gamma);
    return ESP_OK;
}

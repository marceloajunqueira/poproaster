/**
 * @file heater_pid.c
 * @brief See header.
 */
#include <stdbool.h>

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

#define PID_NVS_NAMESPACE "roast_cfg"
#define PID_NVS_KEY_KP "pid_kp"
#define PID_NVS_KEY_KI "pid_ki"
#define PID_NVS_KEY_KD "pid_kd"
#define PID_NVS_KEY_MARGIN "pid_margin"

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

/* ---- Thermal-protector observer thresholds -------------------------------
 *
 * Derived from logs/pid_debug_4.csv, not guessed. Scanning that session's
 * MANUAL rows for "heater genuinely driven AND temperature falling fast":
 *
 *   applied >= 50% AND dT/dt <= -1.5 C/s
 *
 * matches ONLY inside the three collapse windows (t=328-342, 410-425,
 * 455-464) and never once during normal heating - where, at the same duty,
 * dT/dt stayed positive (up to +3.4 C/s). The fastest fall recorded while
 * commanded at maximum was -4.5 C/s, so -1.5 sits comfortably between the
 * two populations. Requiring several consecutive ticks rejects single-sample
 * sensor glitches (the MAX6675 is EMA-filtered at alpha=0.25, so a real
 * collapse persists for many ticks while noise does not). */
#define PROTECTOR_MIN_APPLIED_PCT 50
#define PROTECTOR_FALL_RATE_C_PER_S (-1.5f)
#define PROTECTOR_CONFIRM_TICKS 3

/* Once tripped, wait until the measurement has dropped this far below the
 * trip point before resuming control. The bimetal needs real hysteresis to
 * re-close, and resuming too early just walks straight back into it. */
#define PROTECTOR_RECOVER_DROP_C 25.0f

/* After a trip, hold the target this far below the temperature the trip
 * happened at. This is what actually breaks the cycle: the log shows the
 * machine tripping at ~194C twice in 90s because nothing stopped it from
 * aiming right back at the same place. */
#define PROTECTOR_CEILING_MARGIN_C 15.0f

static float s_integral;
static float s_prev_measured_c;
static bool s_has_prev;
static heater_pid_debug_t s_last_debug;

static uint8_t s_last_applied_pct;
static bool s_protector_open;
static uint32_t s_protector_trip_count;
static float s_protector_trip_temp_c;
static float s_protector_ceiling_c;
static uint8_t s_protector_fall_ticks;

static heater_pid_tuning_t s_tuning = {
    .kp = PID_KP_DEFAULT,
    .ki = PID_KI_DEFAULT,
    .kd = PID_KD_DEFAULT,
    .hard_overshoot_margin_c = PID_HARD_OVERSHOOT_MARGIN_DEFAULT_C,
};

void heater_pid_reset(void)
{
    s_integral = 0.0f;
    s_prev_measured_c = 0.0f;
    s_has_prev = false;
    /* Detection state is transient and must not survive a reset (a stale
     * rate across a gap would be meaningless), but the LATCHED protector
     * findings - trip count and learned ceiling - deliberately do survive:
     * they describe the machine, not the current control episode, and
     * clearing them on every session start would let the controller walk
     * back into the same trip. heater_pid_clear_protector_state() is the
     * explicit way to forget them. */
    s_protector_fall_ticks = 0;
    s_last_applied_pct = 0;
}

uint8_t heater_pid_update(float target_temp_c, float measured_temp_c, float dt_s)
{
    if (dt_s <= 0.0f) {
        dt_s = 1.0f;
    }

    /* Rate of change is needed by BOTH the derivative term and the thermal
     * protector observer, so it is computed once up front - before the hard
     * overshoot cutoff, which returns early. */
    float d_measured = s_has_prev ? (measured_temp_c - s_prev_measured_c) / dt_s : 0.0f;
    bool had_prev = s_has_prev;
    s_prev_measured_c = measured_temp_c;
    s_has_prev = true;

    /* ---- Thermal protector observer ----
     * The hardware protectors give no electrical feedback, so the only
     * evidence is "we are genuinely driving the element hard, yet the air
     * temperature is collapsing". See heater_pid.h for the full rationale
     * and the log evidence behind these thresholds. */
    if (!s_protector_open) {
        if (had_prev && s_last_applied_pct >= PROTECTOR_MIN_APPLIED_PCT &&
            d_measured <= PROTECTOR_FALL_RATE_C_PER_S) {
            if (s_protector_fall_ticks < PROTECTOR_CONFIRM_TICKS) {
                s_protector_fall_ticks++;
            }
            if (s_protector_fall_ticks >= PROTECTOR_CONFIRM_TICKS) {
                s_protector_open = true;
                s_protector_trip_count++;
                /* The collapse has already been running for CONFIRM_TICKS, so
                 * reconstruct roughly where it started rather than recording
                 * the (already much lower) current reading. */
                s_protector_trip_temp_c = measured_temp_c - (d_measured * (float)PROTECTOR_CONFIRM_TICKS * dt_s);
                s_protector_ceiling_c = s_protector_trip_temp_c - PROTECTOR_CEILING_MARGIN_C;
                ESP_LOGW(TAG,
                         "Thermal protector appears OPEN (trip #%u): fell %.1f C/s at %u%% duty, onset ~%.1f C. "
                         "Backing off and capping target at %.1f C.",
                         (unsigned)s_protector_trip_count, (double)d_measured, (unsigned)s_last_applied_pct,
                         (double)s_protector_trip_temp_c, (double)s_protector_ceiling_c);
            }
        } else {
            s_protector_fall_ticks = 0;
        }
    }

    if (s_protector_open) {
        /* Command nothing at all until the tunnel has cooled well below the
         * trip point. Two reasons: driving a coil whose circuit is open is
         * pointless, and - the important one - it guarantees we are NOT
         * sitting at 100% demand at the moment the bimetal re-closes, which
         * is what turned a single trip into a repeating cycle in the log. */
        s_integral = 0.0f;
        if (measured_temp_c <= s_protector_trip_temp_c - PROTECTOR_RECOVER_DROP_C) {
            s_protector_open = false;
            s_protector_fall_ticks = 0;
            ESP_LOGI(TAG, "Thermal protector recovered at %.1f C; resuming control under a %.1f C ceiling",
                     (double)measured_temp_c, (double)s_protector_ceiling_c);
        }
        s_last_debug.error_c = target_temp_c - measured_temp_c;
        s_last_debug.p_term = 0.0f;
        s_last_debug.i_term = 0.0f;
        s_last_debug.d_term = 0.0f;
        s_last_debug.raw_output = 0.0f;
        s_last_debug.hard_cutoff = true;
        s_last_debug.protector_open = true;
        return 0;
    }

    /* Never aim above a ceiling learned from a previous trip - otherwise the
     * controller simply drives back into the protector every time. */
    if (s_protector_ceiling_c > 0.0f && target_temp_c > s_protector_ceiling_c) {
        target_temp_c = s_protector_ceiling_c;
    }

    float error = target_temp_c - measured_temp_c;

    if (error <= -s_tuning.hard_overshoot_margin_c) {
        /* Already meaningfully over target - cut immediately and reset the
         * integral so there's no leftover windup once temperature comes
         * back down toward target. */
        s_integral = 0.0f;
        s_last_debug.error_c = error;
        s_last_debug.p_term = 0.0f;
        s_last_debug.i_term = 0.0f;
        s_last_debug.d_term = 0.0f;
        s_last_debug.raw_output = 0.0f;
        s_last_debug.hard_cutoff = true;
        s_last_debug.protector_open = false;
        return 0;
    }

    float p_term = s_tuning.kp * error;
    float d_term = -s_tuning.kd * d_measured;

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
    s_last_debug.protector_open = false;

    return (uint8_t)(output + 0.5f);
}

void heater_pid_note_applied_pct(uint8_t applied_pct)
{
    s_last_applied_pct = applied_pct;
}

void heater_pid_get_protector_status(heater_pid_protector_status_t *out)
{
    if (out != NULL) {
        out->open = s_protector_open;
        out->trip_count = s_protector_trip_count;
        out->last_trip_temp_c = s_protector_trip_temp_c;
        out->ceiling_c = s_protector_ceiling_c;
    }
}

void heater_pid_clear_protector_state(void)
{
    s_protector_open = false;
    s_protector_fall_ticks = 0;
    s_protector_trip_count = 0;
    s_protector_trip_temp_c = 0.0f;
    s_protector_ceiling_c = 0.0f;
    ESP_LOGI(TAG, "Thermal protector observer state cleared by operator");
}

void heater_pid_get_last_debug(heater_pid_debug_t *out)
{
    if (out != NULL) {
        *out = s_last_debug;
    }
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

    /* An integral accumulated under the old gains has no meaning under the
     * new ones - carrying it over would produce a spurious output step right
     * after the change. */
    heater_pid_reset();

    ESP_LOGI(TAG, "PID tuning set: Kp=%.4f Ki=%.4f Kd=%.4f margin=%.1fC%s", (double)s_tuning.kp,
             (double)s_tuning.ki, (double)s_tuning.kd, (double)s_tuning.hard_overshoot_margin_c,
             persist ? " (persisted)" : "");

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
    nvs_close(nvs);

    ESP_LOGI(TAG, "PID tuning loaded: Kp=%.4f Ki=%.4f Kd=%.4f margin=%.1fC", (double)s_tuning.kp,
             (double)s_tuning.ki, (double)s_tuning.kd, (double)s_tuning.hard_overshoot_margin_c);
    return ESP_OK;
}

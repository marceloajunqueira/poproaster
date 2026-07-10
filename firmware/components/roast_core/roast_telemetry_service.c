/**
 * @file roast_telemetry_service.c
 * @brief See header for design rationale.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "hal/max6675.h"
#include "hal/fan_pwm.h"
#include "hal/ssr_heater.h"
#include "safety/safety_manager.h"
#include "roast_core/ror_calculator.h"
#include "roast_core/dtr_calculator.h"
#include "roast_core/roast_events.h"
#include "roast_core/roast_telemetry_service.h"
#include "storage/session_store.h"
#include "storage/profile_store.h"

static const char *TAG = "roast_telemetry_svc";

#define ROR_SMOOTHING_WINDOW_MS 30000
#define SAMPLE_PERIOD_US (500 * 1000)

/* Automatic Turning Point detection (per operator preference: TP is a pure
 * temperature-curve feature - the coolest moment right after CHARGE, before
 * BT starts climbing again - so it doesn't need a manual button; this
 * service just watches for it). TP_RISE_HYSTERESIS_C/TP_RISE_SAMPLES_NEEDED
 * require a small, SUSTAINED rise (not just one noisy sample) above the
 * observed minimum before committing to where that minimum actually was. */
#define TP_RISE_HYSTERESIS_C 0.3f
#define TP_RISE_SAMPLES_NEEDED 4 /* ~2s at the 500ms sample period */

/* Coarse (~10s-spaced) in-RAM point buffer for the web dashboard's mid-roast
 * chart backfill (roast_telemetry_service_get_live_chart_points()) - see the
 * header doc comment for why this replaced an earlier file-reading design. */
#define LIVE_CHART_SAMPLE_PERIOD_MS 10000

static ror_calculator_handle_t s_ror_handle;
static dtr_calculator_state_t s_dtr_state;
static char s_recording_session_id[SESSION_STORE_ID_MAX_LEN];
static bool s_recording_active = false;
static esp_timer_handle_t s_timer;
static batch_record_t s_pending_batch;

static roast_phase_t s_tp_last_phase = ROAST_PHASE_IDLE;
static bool s_tp_detected = false;
static float s_tp_min_temp_c = 0.0f;
static int64_t s_tp_min_elapsed_ms = 0;
static int s_tp_rise_count = 0;

static live_chart_point_t s_live_chart_points[LIVE_CHART_MAX_POINTS];
static size_t s_live_chart_point_count = 0;
static int64_t s_live_chart_last_stored_ms = 0;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static roast_telemetry_snapshot_t s_snapshot;

static bool phase_is_idle_like(roast_phase_t phase)
{
    return phase == ROAST_PHASE_IDLE || phase == ROAST_PHASE_COMPLETED || phase == ROAST_PHASE_ABORTED;
}

/* Resets/updates the Turning Point tracker and marks the event once found.
 * Tracking (re)starts fresh every time PREHEAT->ROASTING happens (that's
 * CHARGE, session_sm_confirm_charge()'s elapsed_ms=0 reference) and stops
 * once found for this roast - never re-arms mid-roast. */
static void update_turning_point_detection(const roast_session_t *session, float bt, bool sensor_valid)
{
    bool just_charged = (session->phase == ROAST_PHASE_ROASTING && s_tp_last_phase != ROAST_PHASE_ROASTING &&
                          s_tp_last_phase != ROAST_PHASE_DEVELOPMENT);
    if (just_charged) {
        s_tp_detected = false;
        s_tp_rise_count = 0;
        s_tp_min_temp_c = 1000.0f; /* sentinel - first valid sample after CHARGE always seeds the real minimum */
        s_tp_min_elapsed_ms = session->elapsed_ms;
    }
    s_tp_last_phase = session->phase;

    if (s_tp_detected || !sensor_valid ||
        (session->phase != ROAST_PHASE_ROASTING && session->phase != ROAST_PHASE_DEVELOPMENT)) {
        return;
    }

    if (bt <= s_tp_min_temp_c) {
        s_tp_min_temp_c = bt;
        s_tp_min_elapsed_ms = session->elapsed_ms;
        s_tp_rise_count = 0;
    } else if (bt >= s_tp_min_temp_c + TP_RISE_HYSTERESIS_C) {
        s_tp_rise_count++;
        if (s_tp_rise_count >= TP_RISE_SAMPLES_NEEDED) {
            s_tp_detected = true;
            roast_events_mark_at(ROAST_EVENT_TURNING_POINT, s_tp_min_elapsed_ms);
        }
    } else {
        s_tp_rise_count = 0; /* fluctuating within the hysteresis band - not yet a clear/sustained rise */
    }
}

static void sample_timer_cb(void *arg)
{
    (void)arg;

    max6675_sample_t sample;
    bool sensor_valid = (max6675_read(&sample) == ESP_OK && sample.quality == MAX6675_QUALITY_VALID);

    /* The Safety Manager keeps its OWN sensor-valid/last-BT state (used to
     * gate heater-on requests, FR-006) fed exclusively through this call -
     * without it, s_last_sensor_valid there stays permanently false and
     * every heater-on request gets silently rejected, no matter what the
     * curve follower/PID computes. */
    safety_manager_on_temperature_sample(sensor_valid ? sample.bean_temp_c : 0.0f, sensor_valid);

    const roast_session_t *session = session_sm_get_state();
    bool active = !phase_is_idle_like(session->phase);

    float bt = sensor_valid ? sample.bean_temp_c : 0.0f;
    update_turning_point_detection(session, bt, sensor_valid);
    if (sensor_valid && active) {
        ror_calculator_add_sample(s_ror_handle, sample.bean_temp_c, esp_timer_get_time() / 1000);
    }
    float ror_val = active ? ror_calculator_get_ror_c_per_min(s_ror_handle) : 0.0f;
    float dtr_pct = dtr_calculator_get_pct(&s_dtr_state, session->elapsed_ms);
    int fan_pct = fan_pwm_get_pct();
    int heater_pct = ssr_heater_get_duty_pct();

    /* Record a telemetry line for the session history/review screen (T022)
     * while the roast is active and a recording session is open. Fan/heater
     * don't depend on the BT sensor at all, so recording must not be
     * gated on sensor_valid - doing so used to silently skip EVERY record
     * (fan/heater included) whenever the thermocouple was disconnected/
     * invalid, leaving History completely empty instead of just missing
     * the BT column. `bt` already safely falls back to 0.0f above when the
     * sensor is invalid. */
    if (active && s_recording_active) {
        char record[128];
        snprintf(record, sizeof(record), "{\"t\":%lld,\"bt\":%.1f,\"ror\":%.1f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                 (long long)session->elapsed_ms, bt, ror_val, fan_pct, heater_pct, (int)session->phase);
        session_store_append_record(s_recording_session_id, record);
    }
    if (!active && s_recording_active) {
        session_store_finalize_session(s_recording_session_id);
        s_recording_active = false;
    }

    /* Coarse in-RAM point for the web dashboard's mid-roast backfill (see
     * LIVE_CHART_SAMPLE_PERIOD_MS above) - independent of the full-
     * resolution .jsonl recording, never touches the filesystem. */
    if (active && sensor_valid && s_live_chart_point_count < LIVE_CHART_MAX_POINTS &&
        (session->elapsed_ms - s_live_chart_last_stored_ms) >= LIVE_CHART_SAMPLE_PERIOD_MS) {
        portENTER_CRITICAL(&s_lock);
        s_live_chart_points[s_live_chart_point_count].elapsed_ms = session->elapsed_ms;
        s_live_chart_points[s_live_chart_point_count].bean_temp_c = bt;
        s_live_chart_point_count++;
        portEXIT_CRITICAL(&s_lock);
        s_live_chart_last_stored_ms = session->elapsed_ms;
    }

    portENTER_CRITICAL(&s_lock);
    s_snapshot.sensor_valid = sensor_valid;
    s_snapshot.bean_temp_c = bt;
    s_snapshot.ror_c_per_min = ror_val;
    s_snapshot.dtr_pct = dtr_pct;
    s_snapshot.fan_pct = fan_pct;
    s_snapshot.heater_pct = heater_pct;
    s_snapshot.phase = session->phase;
    s_snapshot.paused = session->paused;
    s_snapshot.elapsed_ms = session->elapsed_ms;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t roast_telemetry_service_init(void)
{
    s_ror_handle = ror_calculator_create(ROR_SMOOTHING_WINDOW_MS);
    dtr_calculator_reset(&s_dtr_state);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.dtr_pct = -1.0f;

    const esp_timer_create_args_t args = {
        .callback = sample_timer_cb,
        .name = "roast_telemetry",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, SAMPLE_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Telemetry service started");
    return ESP_OK;
}

void roast_telemetry_service_get_snapshot(roast_telemetry_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

void roast_telemetry_service_on_roast_started(void)
{
    ror_calculator_reset(s_ror_handle);
    dtr_calculator_reset(&s_dtr_state);

    /* Reset the live-chart backfill buffer - `-LIVE_CHART_SAMPLE_PERIOD_MS`
     * (rather than 0) makes the very first tick after CHARGE immediately
     * eligible to store a point (elapsed_ms=0 - (-period) >= period), so
     * the backfilled curve starts right from the beginning instead of
     * waiting a full sample period for its first point. */
    portENTER_CRITICAL(&s_lock);
    s_live_chart_point_count = 0;
    portEXIT_CRITICAL(&s_lock);
    s_live_chart_last_stored_ms = -LIVE_CHART_SAMPLE_PERIOD_MS;

    if (session_store_begin_session(s_recording_session_id, sizeof(s_recording_session_id)) == ESP_OK) {
        s_recording_active = true;

        /* Per operator preference: no RTC/NTP, keep it simple - a global
         * sequential roast number (persisted in NVS) doubles as the
         * session's human-friendly name, alongside the selected preset's
         * name. Also snapshot the whole profile (FR-034) so later deleting
         * or editing it never affects this session's stored history/chart. */
        session_meta_t meta = {0};
        session_store_next_roast_number(&meta.roast_number);
        meta.has_profile = (profile_store_get_selected(&meta.profile) == ESP_OK);
        meta.batch = s_pending_batch;
        session_store_save_meta(s_recording_session_id, &meta);
    } else {
        ESP_LOGW(TAG, "session_store_begin_session failed - history won't be recorded for this roast");
        s_recording_active = false;
    }
}

void roast_telemetry_service_mark_first_crack(void)
{
    const roast_session_t *session = session_sm_get_state();
    dtr_calculator_mark_first_crack(&s_dtr_state, session->elapsed_ms);
}

void roast_telemetry_service_record_event(int64_t elapsed_ms, int event_type)
{
    if (!s_recording_active) {
        return;
    }
    char record[64];
    snprintf(record, sizeof(record), "{\"t\":%lld,\"ev\":%d}", (long long)elapsed_ms, event_type);
    session_store_append_record(s_recording_session_id, record);
}

void roast_telemetry_service_set_pending_batch_info(const batch_record_t *batch)
{
    if (batch == NULL) {
        return;
    }
    s_pending_batch = *batch;
}

void roast_telemetry_service_get_pending_batch_info(batch_record_t *out_batch)
{
    if (out_batch == NULL) {
        return;
    }
    *out_batch = s_pending_batch;
}

bool roast_telemetry_service_get_recording_session_id(char *out, size_t out_len)
{
    if (!s_recording_active || out == NULL || out_len == 0) {
        return false;
    }
    strncpy(out, s_recording_session_id, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

size_t roast_telemetry_service_get_live_chart_points(live_chart_point_t *out, size_t max)
{
    if (out == NULL) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    size_t n = (s_live_chart_point_count < max) ? s_live_chart_point_count : max;
    memcpy(out, s_live_chart_points, n * sizeof(live_chart_point_t));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

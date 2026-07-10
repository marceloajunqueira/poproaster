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
#include "roast_core/roast_telemetry_service.h"
#include "storage/session_store.h"
#include "storage/profile_store.h"

static const char *TAG = "roast_telemetry_svc";

#define ROR_SMOOTHING_WINDOW_MS 30000
#define SAMPLE_PERIOD_US (500 * 1000)

static ror_calculator_handle_t s_ror_handle;
static dtr_calculator_state_t s_dtr_state;
static char s_recording_session_id[SESSION_STORE_ID_MAX_LEN];
static bool s_recording_active = false;
static esp_timer_handle_t s_timer;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static roast_telemetry_snapshot_t s_snapshot;

static bool phase_is_idle_like(roast_phase_t phase)
{
    return phase == ROAST_PHASE_IDLE || phase == ROAST_PHASE_COMPLETED || phase == ROAST_PHASE_ABORTED;
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

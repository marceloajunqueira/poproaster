/**
 * @file fan_failure_detector.c
 * @brief See header.
 */
#include "esp_log.h"
#include "esp_timer.h"

#include "roast_core/roast_telemetry_service.h"
#include "safety/fan_failure_detector.h"
#include "safety/safety_manager.h"

static const char *TAG = "fan_failure_detector";

#define CHECK_PERIOD_US (1000 * 1000)

/* Implausibly fast BT rise for this hardware while the heater is on - a
 * strong indirect signal that the commanded fan isn't actually moving air
 * (stuck/disconnected fan traps heat near the element). Tunable later if
 * real-world roasts show false positives/negatives. */
#define ROR_ANOMALY_THRESHOLD_C_PER_MIN 60.0f

static esp_timer_handle_t s_timer;

static void check_timer_cb(void *arg)
{
    (void)arg;

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    bool actively_heating = (snap.heater_pct > 0) &&
                            (snap.phase == ROAST_PHASE_ROASTING || snap.phase == ROAST_PHASE_DEVELOPMENT);
    if (!actively_heating || !snap.sensor_valid) {
        return;
    }

    if (snap.ror_c_per_min > ROR_ANOMALY_THRESHOLD_C_PER_MIN) {
        ESP_LOGW(TAG, "Abnormal RoR (%.1f C/min) with fan commanded at %d%% while heating - "
                      "reporting indirect fan failure (FR-030)",
                 snap.ror_c_per_min, snap.fan_pct);
        safety_manager_report_indirect_fan_failure();
    }
}

esp_err_t fan_failure_detector_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = check_timer_cb,
        .name = "fan_failure_detector",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, CHECK_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Fan failure detector started (RoR threshold=%.0f C/min)", ROR_ANOMALY_THRESHOLD_C_PER_MIN);
    return ESP_OK;
}

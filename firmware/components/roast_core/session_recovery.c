/**
 * @file session_recovery.c
 * @brief See header.
 */
#include "esp_log.h"

#include "hal/fan_pwm.h"
#include "hal/max6675.h"
#include "hal/ssr_heater.h"
#include "safety/safety_manager.h"
#include "roast_core/session_recovery.h"
#include "roast_core/session_state_machine.h"

static const char *TAG = "session_recovery";

esp_err_t session_recovery_try_resume(void)
{
    session_snapshot_t snap;
    if (!session_sm_has_pending_recovery(&snap)) {
        ESP_LOGI(TAG, "No interrupted session to recover");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Power-loss recovery: found interrupted session '%s' (phase=%d, elapsed=%lldms)",
             snap.session_id, (int)snap.phase, (long long)snap.elapsed_ms);

    /* FR-022: re-validate ventilation minimum + sensor validity BEFORE
     * reactivating anything. The heater is already off (boot default) and
     * is never auto-turned-on here regardless - only fan/alarm state react
     * to this check. */
    max6675_sample_t sample;
    bool sensor_valid = (max6675_read(&sample) == ESP_OK && sample.quality == MAX6675_QUALITY_VALID);

    session_sm_resume_from_recovery(&snap);

    if (!sensor_valid) {
        ESP_LOGE(TAG, "Recovery: sensor invalid - entering safe state (heater OFF, fan at floor) "
                      "and raising a critical alarm pending operator acknowledgment");
        ssr_heater_force_off();
        fan_pwm_set_pct(SAFETY_FAN_MIN_PCT_DURING_HEAT);
        safety_manager_report_recovery_sensor_failure();
    } else {
        ESP_LOGI(TAG, "Recovery: sensor OK - resuming ventilation at the safety floor; "
                      "heater stays off until the operator issues a new command");
        fan_pwm_set_pct(SAFETY_FAN_MIN_PCT_DURING_HEAT);
    }

    return ESP_OK;
}

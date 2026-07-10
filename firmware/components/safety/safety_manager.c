/**
 * @file safety_manager.c
 * @brief Centralized Safety Manager implementation.
 */
#include "esp_log.h"

#include "safety/safety_manager.h"
#include "hal/ssr_heater.h"
#include "hal/fan_pwm.h"
#include "roast_core/session_state_machine.h"

static const char *TAG = "safety_manager";

static safety_alarm_type_t s_active_alarm = SAFETY_ALARM_NONE;
static bool s_alarm_needs_ack = false;
static bool s_last_sensor_valid = false;
static float s_last_bean_temp_c = 0.0f;

static void raise_critical_alarm(safety_alarm_type_t alarm, const char *reason)
{
    /* Any critical alarm forces the heater off immediately, independent of
     * whatever duty was previously commanded (research.md Decision 4). */
    ssr_heater_force_off();
    s_active_alarm = alarm;
    s_alarm_needs_ack = true; /* FR-029: manual ack always required for critical alarms. */
    session_sm_set_safety_state(ROAST_SAFETY_TRIPPED);
    ESP_LOGE(TAG, "CRITICAL ALARM raised (type=%d): %s - heater forced OFF", (int)alarm, reason);
}

esp_err_t safety_manager_init(void)
{
    s_active_alarm = SAFETY_ALARM_NONE;
    s_alarm_needs_ack = false;
    s_last_sensor_valid = false;
    ESP_LOGI(TAG, "Safety Manager init OK (fan floor=%d%%, cutoff=%.0fC, warn=%.0fC)",
             SAFETY_FAN_MIN_PCT_DURING_HEAT, SAFETY_TEMP_ABSOLUTE_CUTOFF_C, SAFETY_TEMP_WARNING_C);
    return ESP_OK;
}

esp_err_t safety_manager_request_fan_pct(uint8_t pct, safety_cmd_source_t source)
{
    if (s_alarm_needs_ack) {
        ESP_LOGW(TAG, "Fan request from source=%d rejected: unacknowledged critical alarm", (int)source);
        return ESP_ERR_INVALID_STATE;
    }

    /* Fan may only be fully stopped (0%) once BT is confirmed below the
     * safe threshold - protects the heating element/chamber from damage if
     * a (now profile-configurable) Cooling segment or an early
     * cancel/emergency-stop tries to cut airflow while still hot. Unknown
     * temperature (invalid sensor) is treated conservatively as "still
     * hot". This is independent of/in addition to the heater-requires-fan
     * floor rule below. */
    if (pct == 0 && (!s_last_sensor_valid || s_last_bean_temp_c >= SAFETY_FAN_STOP_MIN_TEMP_C)) {
        ESP_LOGW(TAG, "Fan-off request from source=%d rejected: BT %.1fC (valid=%d) still above the %.0fC safe-stop threshold",
                 (int)source, s_last_bean_temp_c, (int)s_last_sensor_valid, SAFETY_FAN_STOP_MIN_TEMP_C);
        return ESP_ERR_INVALID_STATE;
    }

    /* FR-004: while heating, never allow the fan below the fixed 30% floor. */
    if (ssr_heater_get_duty_pct() > 0 && pct < SAFETY_FAN_MIN_PCT_DURING_HEAT) {
        ESP_LOGW(TAG, "Fan request %d%% from source=%d rejected: below %d%% floor while heating",
                 pct, (int)source, SAFETY_FAN_MIN_PCT_DURING_HEAT);
        return ESP_ERR_INVALID_STATE;
    }

    return fan_pwm_set_pct(pct);
}

esp_err_t safety_manager_request_heater_pct(uint8_t pct, safety_cmd_source_t source)
{
    if (s_alarm_needs_ack) {
        ESP_LOGW(TAG, "Heater request from source=%d rejected: unacknowledged critical alarm", (int)source);
        return ESP_ERR_INVALID_STATE;
    }

    if (pct > 0) {
        /* FR-003: heater must never activate without the fan already running at/above the floor. */
        if (fan_pwm_get_pct() < SAFETY_FAN_MIN_PCT_DURING_HEAT) {
            ESP_LOGW(TAG, "Heater request %d%% from source=%d rejected: fan below %d%% floor",
                     pct, (int)source, SAFETY_FAN_MIN_PCT_DURING_HEAT);
            return ESP_ERR_INVALID_STATE;
        }
        /* FR-006: never re-enable heating on a known-invalid sensor reading. */
        if (!s_last_sensor_valid) {
            ESP_LOGW(TAG, "Heater request %d%% from source=%d rejected: last sensor reading invalid",
                     pct, (int)source);
            return ESP_ERR_INVALID_STATE;
        }
    }

    return ssr_heater_set_duty_pct(pct);
}

void safety_manager_on_temperature_sample(float bean_temp_c, bool sensor_valid)
{
    s_last_bean_temp_c = bean_temp_c;
    s_last_sensor_valid = sensor_valid;

    if (!sensor_valid) {
        if (ssr_heater_get_duty_pct() > 0) {
            raise_critical_alarm(SAFETY_ALARM_SENSOR_FAILURE, "invalid/stale temperature reading during heating");
        }
        return;
    }

    if (bean_temp_c >= SAFETY_TEMP_ABSOLUTE_CUTOFF_C) {
        raise_critical_alarm(SAFETY_ALARM_TEMP_ABSOLUTE_CUTOFF, "260C absolute safety cutoff reached");
        return;
    }

    if (bean_temp_c >= SAFETY_TEMP_WARNING_C && s_active_alarm == SAFETY_ALARM_NONE) {
        /* FR-026: 240C is a non-blocking warning, not a hard cutoff - logged
         * so the UI layer can surface it, but does not force the heater off
         * or require acknowledgment on its own. */
        ESP_LOGW(TAG, "Temperature warning: %.1fC >= %.0fC (approaching absolute cutoff)",
                 bean_temp_c, SAFETY_TEMP_WARNING_C);
    }
}

void safety_manager_report_indirect_fan_failure(void)
{
    raise_critical_alarm(SAFETY_ALARM_FAN_FAILURE_INDIRECT,
                         "RoR anomaly inconsistent with commanded fan speed (FR-030)");
}

void safety_manager_report_duration_exceeded(void)
{
    /* FR-033: watchdog forces cooling, not a full alarm-gated stop, but the
     * critical alarm + ack is still required per FR-029's list including
     * this case, and the caller (duration_watchdog.c) also drives the
     * automatic transition to COOLING via session_sm_start_cooling(). */
    raise_critical_alarm(SAFETY_ALARM_DURATION_WATCHDOG, "maximum roast duration exceeded");
}

void safety_manager_report_recovery_sensor_failure(void)
{
    raise_critical_alarm(SAFETY_ALARM_SENSOR_FAILURE, "sensor invalid at power-loss recovery");
}

esp_err_t safety_manager_emergency_stop(void)
{
    raise_critical_alarm(SAFETY_ALARM_EMERGENCY_STOP, "operator-triggered Emergency Stop");
    return ESP_OK;
}

esp_err_t safety_manager_acknowledge_alarm(void)
{
    if (!s_alarm_needs_ack) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Critical alarm (type=%d) acknowledged by operator", (int)s_active_alarm);
    s_alarm_needs_ack = false;
    s_active_alarm = SAFETY_ALARM_NONE;
    session_sm_set_safety_state(ROAST_SAFETY_OK);
    return ESP_OK;
}

safety_alarm_type_t safety_manager_get_active_alarm(bool *out_needs_ack)
{
    if (out_needs_ack) {
        *out_needs_ack = s_alarm_needs_ack;
    }
    return s_active_alarm;
}

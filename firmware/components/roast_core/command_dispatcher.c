/**
 * @file command_dispatcher.c
 * @brief Single entry point for fan/heater/alarm commands (see header for the
 *        two rules it enforces: Safety Manager validation + Profile-mode
 *        Artisan read-only gate).
 */
#include "esp_log.h"

#include "roast_core/command_dispatcher.h"
#include "roast_core/session_state_machine.h"

static const char *TAG = "command_dispatcher";

static safety_cmd_source_t s_last_applied_source = SAFETY_CMD_SOURCE_DISPLAY;

/**
 * US3 Acceptance Scenario 4: while the active session is in
 * ROAST_MODE_PROFILE, control commands arriving from Artisan are ignored
 * (the Artisan connection is read-only in Profile mode); telemetry keeps
 * flowing regardless. Display/web commands are never gated here - they are
 * allowed as manual overrides during Profile mode (T035).
 */
static bool is_command_ignored_by_mode_gate(safety_cmd_source_t source)
{
    if (source != SAFETY_CMD_SOURCE_ARTISAN) {
        return false;
    }
    const roast_session_t *session = session_sm_get_state();
    return session->control_mode == ROAST_MODE_PROFILE;
}

esp_err_t command_dispatcher_init(void)
{
    s_last_applied_source = SAFETY_CMD_SOURCE_DISPLAY;
    ESP_LOGI(TAG, "Command dispatcher init OK");
    return ESP_OK;
}

esp_err_t command_dispatcher_set_fan_pct(uint8_t pct, safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Fan request from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }

    esp_err_t err = safety_manager_request_fan_pct(pct, source);
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_set_heater_pct(uint8_t pct, safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Heater request from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }

    esp_err_t err = safety_manager_request_heater_pct(pct, source);
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_emergency_stop(safety_cmd_source_t source)
{
    /* FR-027: Emergency Stop is never gated by control mode or source. */
    esp_err_t err = safety_manager_emergency_stop();
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }

    /* Per operator requirement: Emergency Stop also cancels any active
     * roast session, immediately (no COOLING waiting period) - the
     * dashboard reverts to "Start Roast" right away. session_sm_abort() is
     * a no-op (ESP_ERR_INVALID_STATE, safely ignored) if there was no
     * active session to begin with. */
    session_sm_abort("Emergency Stop activated");

    /* Best-effort attempt to fully stop the fan too - the alarm this just
     * raised requires acknowledgment first (safety_manager_request_fan_pct
     * rejects ANY fan command while unacknowledged), so in practice this
     * only takes effect after the operator acks the alarm and it's already
     * safe to do so; harmless no-op otherwise. */
    command_dispatcher_set_fan_pct(0, source);

    return err;
}

esp_err_t command_dispatcher_acknowledge_alarm(safety_cmd_source_t source)
{
    /* FR-029: alarm acknowledgment is never gated by control mode or source. */
    esp_err_t err = safety_manager_acknowledge_alarm();
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_pause_session(safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Pause request from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }
    esp_err_t err = session_sm_pause();
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_resume_session(safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Resume request from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }
    esp_err_t err = session_sm_resume();
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_confirm_charge(safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Charge confirmation from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }
    esp_err_t err = session_sm_confirm_charge();
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

esp_err_t command_dispatcher_cancel_session(safety_cmd_source_t source)
{
    /* Operator feedback: Cancel must actually stop the session right away
     * (dashboard shows "Start Roast" again immediately), not sit showing
     * "Cooling" indefinitely - session_sm_abort() transitions straight to
     * the terminal ABORTED phase (heater already forced off as part of
     * that). */
    esp_err_t err = session_sm_abort("Cancelled by operator");
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }

    /* Best-effort attempt to also fully stop the fan - safety_manager's
     * existing SAFETY_FAN_STOP_MIN_TEMP_C rule rejects this (leaving the
     * fan running at its last commanded speed) while BT is still >=100C or
     * the sensor is invalid, so the chamber keeps getting safely cooled by
     * airflow even though the session/UI already looks idle again; once
     * it's actually safe, this succeeds and the fan turns off. */
    command_dispatcher_set_fan_pct(0, source);

    return err;
}

esp_err_t command_dispatcher_switch_to_manual_artisan(bool operator_confirmed_irreversible, safety_cmd_source_t source)
{
    esp_err_t err = session_sm_switch_to_manual_artisan(operator_confirmed_irreversible);
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
    return err;
}

safety_cmd_source_t command_dispatcher_get_last_applied_source(void)
{
    return s_last_applied_source;
}

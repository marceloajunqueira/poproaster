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
     * roast session, but rather than terminating it outright, this forces
     * a safe transition into COOLING first (heater already off via
     * safety_manager_emergency_stop() above; the fan is kept running) - see
     * session_sm_cancel() and the SAFETY_FAN_STOP_MIN_TEMP_C rule in
     * safety_manager.h. The session finalizes as ABORTED once
     * profile_curve_follower.c decides cooling is done. session_sm_cancel()
     * is a no-op (ESP_ERR_INVALID_STATE, safely ignored) if there was no
     * active session to begin with. */
    session_sm_cancel("Emergency Stop activated");

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
    /* Rather than terminating outright, this forces a safe transition into
     * COOLING (fan kept running) - see session_sm_cancel() and the
     * SAFETY_FAN_STOP_MIN_TEMP_C rule in safety_manager.h. The session
     * finalizes as ABORTED once profile_curve_follower.c decides cooling
     * is done. */
    esp_err_t err = session_sm_cancel("Cancelled by operator");
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }
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

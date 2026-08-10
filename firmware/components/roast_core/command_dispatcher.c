/**
 * @file command_dispatcher.c
 * @brief Single entry point for fan/heater/alarm commands (see header for the
 *        two rules it enforces: Safety Manager validation + Profile-mode
 *        Artisan read-only gate).
 */
#include "esp_log.h"

#include "roast_core/command_dispatcher.h"
#include "roast_core/session_state_machine.h"
#include "roast_core/profile_curve_follower.h"

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

    /* NOTE: the fan is already force-killed directly inside
     * safety_manager_emergency_stop() (bypassing the usual
     * safety_manager_request_fan_pct() anti-scorch gate) - Emergency Stop
     * is meant to cut everything at once, unlike other alarms which
     * deliberately leave the fan running. No extra fan command needed here. */

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
    /* Two-stage per operator request, but ONLY meaningful while
     * ROASTING/DEVELOPMENT (a real roast in progress, something worth
     * extending a little before ending): jump to COOLING first (same
     * transition the profile's own trailing Cooling segment uses), a
     * second press while already COOLING finalizes right away instead of
     * waiting out the rest of the cooling curve. Any OTHER active phase
     * (PREHEAT - nothing has been charged/roasted yet, so there's no
     * "Cooling" to extend into) is an immediate hard stop, same as before -
     * BUG FIX: session_sm_start_cooling() itself rejects any phase other
     * than ROASTING/DEVELOPMENT, so calling it unconditionally here used to
     * silently do NOTHING at all during PREHEAT (operator-reported: Cancel
     * button "didn't work" while still preheating). */
    roast_phase_t phase = session_sm_get_state()->phase;
    bool go_to_cooling = (phase == ROAST_PHASE_ROASTING || phase == ROAST_PHASE_DEVELOPMENT);
    esp_err_t err = go_to_cooling ? session_sm_start_cooling() : session_sm_abort("Cancelled by operator");
    if (err == ESP_OK) {
        s_last_applied_source = source;
    }

    if (!go_to_cooling) {
        /* Best-effort attempt to also fully stop the fan - safety_manager's
         * existing SAFETY_FAN_STOP_MIN_TEMP_C rule rejects this (leaving the
         * fan running at its last commanded speed) while BT is still >=100C
         * or the sensor is invalid, so the chamber keeps getting safely
         * cooled by airflow even though the session/UI already looks idle
         * again; once it's actually safe, this succeeds and the fan turns
         * off. Deliberately NOT attempted when transitioning INTO Cooling -
         * profile_curve_follower.c's own Cooling fan curve (or its
         * fallback) should drive the fan from there, not have it
         * immediately fought back down to 0. */
        command_dispatcher_set_fan_pct(0, source);
    }

    return err;
}

esp_err_t command_dispatcher_skip_to_next_segment(safety_cmd_source_t source)
{
    if (is_command_ignored_by_mode_gate(source)) {
        ESP_LOGI(TAG, "Next-segment skip from Artisan ignored: session is in Profile mode (read-only)");
        return ESP_OK;
    }
    esp_err_t err = profile_curve_follower_skip_to_next_segment();
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

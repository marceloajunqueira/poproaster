/**
 * @file command_dispatcher.h
 * @brief Single entry point for every fan/heater/alarm command, regardless of
 *        source (display, web, Artisan, or the profile curve follower).
 *
 * All control surfaces (display buttons, web control endpoint, Artisan
 * bridge, profile curve follower) MUST call through here instead of calling
 * safety/safety_manager.h directly. This keeps two cross-cutting rules in a
 * single place:
 *
 *   1. Every fan/heater request is still validated by the Safety Manager
 *      (fan floor, sensor validity, alarm-ack gate, etc. - see safety_manager.h).
 *   2. Multi-source arbitration: per spec.md FR (Session 2026-07-06) "ultimo
 *      comando vence" - any connected source may command freely and the most
 *      recent request wins, EXCEPT that while the active session is in
 *      ROAST_MODE_PROFILE, commands from SAFETY_CMD_SOURCE_ARTISAN are
 *      accepted for telemetry purposes only and silently ignored for control
 *      (US3 Acceptance Scenario 4: Artisan connection is read-only in Profile
 *      mode). Display and web commands are still accepted in Profile mode as
 *      manual overrides (see profile_curve_follower.c, T035).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "safety/safety_manager.h"

esp_err_t command_dispatcher_init(void);

/** Routes a fan speed request through the mode gate and the Safety Manager. */
esp_err_t command_dispatcher_set_fan_pct(uint8_t pct, safety_cmd_source_t source);

/** Routes a heater duty request through the mode gate and the Safety Manager. */
esp_err_t command_dispatcher_set_heater_pct(uint8_t pct, safety_cmd_source_t source);

/** FR-027: Emergency Stop is always accepted, regardless of control mode or source. */
esp_err_t command_dispatcher_emergency_stop(safety_cmd_source_t source);

/** FR-029: alarm acknowledgment is always accepted, regardless of control mode or source. */
esp_err_t command_dispatcher_acknowledge_alarm(safety_cmd_source_t source);

/**
 * FR-001: operator-initiated pause/resume of the active roast session (distinct
 * from the power-loss auto-resume in session_recovery.c, T026). Subject to the
 * same Profile-mode-Artisan-read-only gate as fan/heater commands.
 */
esp_err_t command_dispatcher_pause_session(safety_cmd_source_t source);
esp_err_t command_dispatcher_resume_session(safety_cmd_source_t source);

/** T036: operator-confirmed CHARGE event (PREHEAT -> ROASTING), subject to the same Profile-mode-Artisan-read-only gate as the other lifecycle commands. */
esp_err_t command_dispatcher_confirm_charge(safety_cmd_source_t source);

/**
 * Operator-initiated cancellation of the active roast (distinct from
 * Emergency Stop: this is a calm, deliberate "abandon this roast and let me
 * start over" action, not a safety trip - it does NOT raise a critical
 * alarm or require acknowledgment). Never gated by control mode/source,
 * same as the other lifecycle-ending commands.
 */
esp_err_t command_dispatcher_cancel_session(safety_cmd_source_t source);

/**
 * T046: switches an active PROFILE-mode session to MANUAL_ARTISAN
 * (irreversible for the session - see session_sm_switch_to_manual_artisan()).
 * Never gated by the Profile-mode-Artisan-read-only rule itself (that rule
 * only applies to fan/heater/pause/etc. commands while ALREADY in Profile
 * mode - this is the one command that's explicitly meant to end Profile
 * mode). `operator_confirmed_irreversible` must be true or the switch is
 * refused (ESP_ERR_INVALID_ARG) - callers (display/web) must get an
 * explicit operator confirmation before calling this with true.
 */
esp_err_t command_dispatcher_switch_to_manual_artisan(bool operator_confirmed_irreversible, safety_cmd_source_t source);

/** Returns the source of the last command that was actually applied (not ignored), for telemetry (TelemetrySample.commandSource). */
safety_cmd_source_t command_dispatcher_get_last_applied_source(void);

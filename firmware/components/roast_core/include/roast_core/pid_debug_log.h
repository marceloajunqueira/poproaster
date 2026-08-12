/**
 * @file pid_debug_log.h
 * @brief Operator-requested PID tuning log: records every heater_pid_update()
 *        call profile_curve_follower.c makes (Profile-mode segments AND
 *        Manual mode's Target Temp) to a CSV file on the "storage" LittleFS
 *        partition, downloadable from the web Diagnostics page
 *        (GET /api/pid_log/download) for offline analysis/tuning.
 *
 * Only logs while there's an actual nonzero target temperature - avoids
 * silently filling storage while the roaster just sits idle (Manual mode's
 * PID runs on every follower tick regardless of session state as of the
 * "Manual control must be free" fix, so gating on a real target is what
 * keeps this from logging 24/7 forever).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "roast_core/session_state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ensures the log file exists (does NOT clear any previous content - the operator may want to keep logging across reboots until they explicitly clear it). */
esp_err_t pid_debug_log_init(void);

/**
 * Appends one CSV row capturing the full state of a single heater_pid_update()
 * call plus surrounding context (fan, phase, Max Heater Power scaling) - call
 * right after each heater_pid_update() call in profile_curve_follower.c.
 * `mode_str` is a short label ("PROFILE" or "MANUAL"). Auto-truncates and
 * restarts the file once it exceeds PID_DEBUG_LOG_MAX_BYTES, to bound
 * storage usage on this small (896KB) LittleFS partition, which is shared
 * with roast history recordings.
 */
void pid_debug_log_record(const char *mode_str, roast_phase_t phase, uint32_t elapsed_s, float target_temp_c,
                           float measured_temp_c, bool sensor_valid, uint8_t requested_heater_pct,
                           uint8_t applied_heater_pct, uint8_t target_fan_pct, uint8_t real_fan_pct);

/** Deletes the log file so the next pid_debug_log_record() call starts a fresh, empty log - lets the operator isolate a single test run. */
esp_err_t pid_debug_log_clear(void);

/** Returns the current log file size in bytes (0 if it doesn't exist yet) - for showing on the Diagnostics page. */
size_t pid_debug_log_get_size(void);

/**
 * Operator-requested (2026-08-12): once tuning has stabilized, many
 * subsequent roasts would otherwise keep writing this log for no reason -
 * lets logging be turned off entirely (pid_debug_log_record() becomes a
 * no-op) without losing the ability to turn it back on for a future tuning
 * session. Persisted to NVS immediately, so the setting survives a reboot
 * and stays off across every future roast until explicitly re-enabled.
 */
void pid_debug_log_set_enabled(bool enabled);

/** Returns whether logging is currently enabled (defaults to true, matching the original always-on behavior, until the operator disables it). */
bool pid_debug_log_is_enabled(void);

#ifdef __cplusplus
}
#endif

/**
 * @file session_state_machine.h
 * @brief RoastSession state machine (phase + control mode).
 *
 * Phases (data-model.md RoastSession.status):
 *   IDLE -> PREHEAT -> ROASTING -> DEVELOPMENT -> COOLING -> COMPLETED
 *   any active phase -> ABORTED (emergency/failure)
 *
 * Control modes (FR-039/FR-040): PROFILE or MANUAL_ARTISAN. A session starts
 * in one of these two modes; it may transition PROFILE -> MANUAL_ARTISAN once
 * (irreversibly) during an active roast, but never back.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ROAST_PHASE_IDLE = 0,
    ROAST_PHASE_PREHEAT,
    ROAST_PHASE_ROASTING,
    ROAST_PHASE_DEVELOPMENT,
    ROAST_PHASE_COOLING,
    ROAST_PHASE_COMPLETED,
    ROAST_PHASE_ABORTED,
} roast_phase_t;

typedef enum {
    ROAST_MODE_PROFILE = 0,
    ROAST_MODE_MANUAL_ARTISAN,
} roast_control_mode_t;

typedef enum {
    ROAST_SAFETY_OK = 0,
    ROAST_SAFETY_WARNING,
    ROAST_SAFETY_TRIPPED,
} roast_safety_state_t;

typedef struct {
    char session_id[40];
    roast_phase_t phase;
    roast_control_mode_t control_mode;
    roast_safety_state_t safety_state;
    bool paused;             /* Operator-initiated pause (FR-001), distinct from ABORTED. */
    bool mode_switch_used;   /* True once PROFILE -> MANUAL_ARTISAN has happened (irreversible). */
    int64_t started_at_ms;
    int64_t elapsed_ms;      /* Time-in-roast, excluding time spent paused - drives the profile curve/segment position, see profile_curve_follower.c. */
    int64_t wall_elapsed_ms; /* Time-in-roast INCLUDING time spent paused - display only (dashboard/web timer), never used to drive the profile curve. */
} roast_session_t;

esp_err_t session_state_machine_init(void);

/** Starts a new session in the given mode (PROFILE requires a profile to already be loaded upstream). */
esp_err_t session_sm_start(roast_control_mode_t mode);

/** FR-001: operator-initiated pause/resume, distinct from power-loss auto-resume (session_recovery.h). */
esp_err_t session_sm_pause(void);
esp_err_t session_sm_resume(void);

/**
 * T036 (minimal): operator-confirmed CHARGE event - transitions PREHEAT ->
 * ROASTING. This is when beans are actually loaded into the drum, so the
 * elapsed-time reference (and therefore the profile curve's own timeline,
 * T034) restarts counting from zero at this moment rather than from
 * session_sm_start() (which only marks "preheat begun"). Returns
 * ESP_ERR_INVALID_STATE if not currently in PREHEAT.
 */
esp_err_t session_sm_confirm_charge(void);

/** Transitions to COOLING - automatically via the profile's own trailing "Cooling" segment(s) (see profile_curve_follower.c), or as the first stage of an operator Cancel (see command_dispatcher_cancel_session()) so the roast can still be extended a little before it actually ends. */
esp_err_t session_sm_start_cooling(void);

/** Finalizes a session that has finished COOLING: always COMPLETED. Called automatically by profile_curve_follower.c once cooling's exit condition is met (profile timeline elapsed, BT below the safe threshold, or a failsafe max duration) - there is no manual "Complete" button (a second operator Cancel press while already COOLING instead finalizes via session_sm_abort(), not this function). */
esp_err_t session_sm_complete(void);

/**
 * Operator-initiated cancellation (Cancel button, second press while
 * already COOLING) or Emergency Stop of an active roast: immediately
 * transitions to ABORTED (heater forced off,
 * same as any other terminal transition) so the dashboard shows "Start
 * Roast" again right away - it does NOT wait through a COOLING grace
 * period. The fan is deliberately left untouched here (whatever it was
 * last commanded to); callers are expected to also attempt a fan-off
 * request afterward (see command_dispatcher_cancel_session()) - the
 * existing SAFETY_FAN_STOP_MIN_TEMP_C rule in safety_manager.h will reject
 * that fan-off request (leaving the fan running) while BT is still >=100C
 * or the sensor is invalid, so the chamber still gets safely cooled by
 * airflow even though the session itself already looks idle. Returns
 * ESP_ERR_INVALID_STATE if there's no active session (already IDLE).
 */
esp_err_t session_sm_abort(const char *reason);

/**
 * FR-040: switches an active PROFILE-mode session to MANUAL_ARTISAN.
 * Irreversible for the session (mode_switch_used latches true); returns
 * ESP_ERR_INVALID_STATE if already in MANUAL_ARTISAN or if not called with
 * the required operator confirmation flag.
 */
esp_err_t session_sm_switch_to_manual_artisan(bool operator_confirmed_irreversible);

/**
 * Operator-initiated "Next segment" skip (profile_curve_follower.c): pushes
 * the profile curve's own timeline (elapsed_ms, NOT wall_elapsed_ms) forward
 * by `delta_ms` - the displayed/wall-clock timer is deliberately unaffected,
 * so the operator can see how much time was actually cut versus the
 * profile's planned duration. Only valid while ROASTING/DEVELOPMENT; rejects
 * delta_ms <= 0.
 */
esp_err_t session_sm_skip_curve_time(int64_t delta_ms);

esp_err_t session_sm_set_safety_state(roast_safety_state_t state);

/** Returns a read-only snapshot of the current session state. */
const roast_session_t *session_sm_get_state(void);

/**
 * FR-022: power-loss auto-resume support. The session state machine
 * periodically persists a compact snapshot to NVS (storage/nvs_store.h)
 * while a session is active; session_recovery.c uses these at boot to
 * detect and resume an interrupted session.
 */
typedef struct {
    char session_id[40];
    roast_phase_t phase;
    roast_control_mode_t control_mode;
    bool paused;
    bool mode_switch_used;
    int64_t elapsed_ms; /* Elapsed time as of the last periodic save, excluding paused time. */
} session_snapshot_t;

/** Reads the last persisted snapshot (if any) without altering live state.
 * Returns false if there's nothing to recover (no snapshot, or the
 * persisted phase was already IDLE/COMPLETED/ABORTED). */
bool session_sm_has_pending_recovery(session_snapshot_t *out);

/** Restores live session state from a snapshot after a power-loss reboot,
 * recomputing started_at_ms so elapsed_ms keeps counting from where it left
 * off (the outage itself is NOT counted as roast time). */
esp_err_t session_sm_resume_from_recovery(const session_snapshot_t *snapshot);

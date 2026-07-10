/**
 * @file session_state_machine.c
 * @brief RoastSession state machine implementation.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "hal/ssr_heater.h"
#include "storage/nvs_store.h"
#include "roast_core/session_state_machine.h"

static const char *TAG = "session_sm";

#define SESSION_SNAPSHOT_NVS_KEY "sess_snap"
#define SNAPSHOT_SAVE_PERIOD_US (10 * 1000 * 1000) /* 10s - bounds how stale elapsed_ms can be after a power loss. */

static roast_session_t s_session;
static int64_t s_pause_started_at_ms = 0;
static int64_t s_total_paused_ms = 0;
static int64_t s_ended_at_ms = 0; /* Set when the session reaches COMPLETED/ABORTED, so elapsed_ms freezes instead of counting against live time. */
static bool s_pending_abort = false; /* Set by session_sm_cancel() (Cancel/Emergency Stop) - once cooling naturally ends, session_sm_complete() finalizes as ABORTED (with s_pending_abort_reason) instead of COMPLETED. */
static char s_pending_abort_reason[64];
static esp_timer_handle_t s_snapshot_timer;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/** FR-022: persists a compact snapshot of the current session to NVS so
 * session_recovery.c can resume it after a power-loss reboot. */
static void persist_snapshot(void)
{
    session_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.session_id, s_session.session_id, sizeof(snap.session_id) - 1);
    snap.phase = s_session.phase;
    snap.control_mode = s_session.control_mode;
    snap.paused = s_session.paused;
    snap.mode_switch_used = s_session.mode_switch_used;
    snap.elapsed_ms = s_session.elapsed_ms;
    nvs_store_set_blob(SESSION_SNAPSHOT_NVS_KEY, &snap, sizeof(snap));
}

static void snapshot_timer_cb(void *arg)
{
    (void)arg;
    if (s_session.phase == ROAST_PHASE_IDLE) {
        return;
    }
    /* Refresh elapsed_ms (session_sm_get_state() also does this as a side
     * effect) before persisting, so the snapshot stays reasonably fresh. */
    session_sm_get_state();
    persist_snapshot();
}

esp_err_t session_state_machine_init(void)
{
    memset(&s_session, 0, sizeof(s_session));
    s_session.phase = ROAST_PHASE_IDLE;
    s_session.safety_state = ROAST_SAFETY_OK;

    const esp_timer_create_args_t args = {
        .callback = snapshot_timer_cb,
        .name = "session_snapshot",
    };
    esp_err_t err = esp_timer_create(&args, &s_snapshot_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create (snapshot) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_snapshot_timer, SNAPSHOT_SAVE_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic (snapshot) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Session state machine init OK (phase=IDLE)");
    return ESP_OK;
}

esp_err_t session_sm_start(roast_control_mode_t mode)
{
    if (s_session.phase != ROAST_PHASE_IDLE && s_session.phase != ROAST_PHASE_COMPLETED &&
        s_session.phase != ROAST_PHASE_ABORTED) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_session, 0, sizeof(s_session));
    uint32_t rnd = esp_random();
    int64_t start_ms = now_ms();
    snprintf(s_session.session_id, sizeof(s_session.session_id), "session_%lld_%08" PRIx32,
             (long long)start_ms, rnd);
    s_session.phase = ROAST_PHASE_PREHEAT;
    s_session.control_mode = mode;
    s_session.safety_state = ROAST_SAFETY_OK;
    s_session.started_at_ms = start_ms;
    s_session.paused = false;
    s_session.mode_switch_used = (mode == ROAST_MODE_MANUAL_ARTISAN);
    s_pause_started_at_ms = 0;
    s_total_paused_ms = 0;
    s_ended_at_ms = 0;
    s_pending_abort = false;
    s_pending_abort_reason[0] = '\0';

    ESP_LOGI(TAG, "Session '%s' started in %s mode", s_session.session_id,
             mode == ROAST_MODE_PROFILE ? "PROFILE" : "MANUAL_ARTISAN");
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_pause(void)
{
    if (s_session.phase == ROAST_PHASE_IDLE || s_session.phase == ROAST_PHASE_COMPLETED ||
        s_session.phase == ROAST_PHASE_ABORTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.paused) {
        return ESP_OK; /* Idempotent. */
    }
    s_session.paused = true;
    s_pause_started_at_ms = now_ms();
    ESP_LOGI(TAG, "Session '%s' paused by operator", s_session.session_id);
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_resume(void)
{
    if (!s_session.paused) {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.paused = false;
    s_total_paused_ms += now_ms() - s_pause_started_at_ms;
    ESP_LOGI(TAG, "Session '%s' resumed by operator", s_session.session_id);
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_confirm_charge(void)
{
    if (s_session.phase != ROAST_PHASE_PREHEAT) {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.phase = ROAST_PHASE_ROASTING;
    /* Elapsed time (and the profile curve's own timeline, T034) starts
     * counting from CHARGE, not from when Start Roast/preheat began -
     * matches how Artisan/roast logs conventionally define t=0. */
    s_session.started_at_ms = now_ms();
    s_total_paused_ms = 0;
    ESP_LOGI(TAG, "Session '%s' CHARGE confirmed: PREHEAT -> ROASTING", s_session.session_id);
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_start_cooling(void)
{
    if (s_session.phase != ROAST_PHASE_ROASTING && s_session.phase != ROAST_PHASE_DEVELOPMENT) {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.phase = ROAST_PHASE_COOLING;
    /* FR-005/T025: Cooling always forces the heater off immediately as part
     * of the transition itself (the profile's own trailing Cooling
     * segment, or session_sm_cancel() below) - never a separate heater-off
     * command. The fan is deliberately left untouched so it keeps running
     * through cooling (profile_curve_follower.c then drives it per the
     * Cooling segment's own curve, or a safe fallback speed). */
    ssr_heater_force_off();
    ESP_LOGI(TAG, "Session '%s' entering COOLING (heater forced off, fan unchanged)", s_session.session_id);
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_cancel(const char *reason)
{
    if (s_session.phase == ROAST_PHASE_IDLE || s_session.phase == ROAST_PHASE_COMPLETED ||
        s_session.phase == ROAST_PHASE_ABORTED) {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(s_pending_abort_reason, reason ? reason : "Cancelled", sizeof(s_pending_abort_reason) - 1);
    s_pending_abort_reason[sizeof(s_pending_abort_reason) - 1] = '\0';
    s_pending_abort = true;

    if (s_session.phase == ROAST_PHASE_COOLING) {
        /* Already cooling (e.g. the profile's own trailing Cooling segment
         * was already running) - just mark the eventual outcome as ABORTED
         * instead of COMPLETED; no phase change needed. */
        ESP_LOGW(TAG, "Session '%s' cancel requested while already COOLING: %s", s_session.session_id,
                 s_pending_abort_reason);
        persist_snapshot();
        return ESP_OK;
    }

    /* PREHEAT/ROASTING/DEVELOPMENT -> force an immediate transition into
     * COOLING instead of stopping outright: heater off right away, but the
     * fan keeps running (see SAFETY_FAN_STOP_MIN_TEMP_C) until
     * profile_curve_follower.c decides it's actually safe/done to finalize
     * via session_sm_complete(). */
    s_session.phase = ROAST_PHASE_COOLING;
    ssr_heater_force_off();
    ESP_LOGW(TAG, "Session '%s' cancelled: %s - entering COOLING (fan stays on until safe)", s_session.session_id,
             s_pending_abort_reason);
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_complete(void)
{
    if (s_session.phase != ROAST_PHASE_COOLING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pending_abort) {
        s_session.phase = ROAST_PHASE_ABORTED;
        ESP_LOGW(TAG, "Session '%s' finished cooling and is ABORTED: %s", s_session.session_id, s_pending_abort_reason);
    } else {
        s_session.phase = ROAST_PHASE_COMPLETED;
        ESP_LOGI(TAG, "Session '%s' COMPLETED", s_session.session_id);
    }
    s_ended_at_ms = now_ms();
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_abort(const char *reason)
{
    if (s_session.phase == ROAST_PHASE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.phase = ROAST_PHASE_ABORTED;
    s_ended_at_ms = now_ms();
    ssr_heater_force_off();
    ESP_LOGW(TAG, "Session '%s' ABORTED: %s", s_session.session_id, reason ? reason : "unspecified");
    persist_snapshot();
    return ESP_OK;
}

esp_err_t session_sm_switch_to_manual_artisan(bool operator_confirmed_irreversible)
{
    if (s_session.control_mode != ROAST_MODE_PROFILE) {
        return ESP_ERR_INVALID_STATE; /* Already MANUAL_ARTISAN or no active session. */
    }
    if (!operator_confirmed_irreversible) {
        ESP_LOGW(TAG, "Refusing mode switch: operator confirmation of irreversibility required (FR-040)");
        return ESP_ERR_INVALID_ARG;
    }
    s_session.control_mode = ROAST_MODE_MANUAL_ARTISAN;
    s_session.mode_switch_used = true;
    ESP_LOGW(TAG, "Session '%s' irreversibly switched PROFILE -> MANUAL_ARTISAN", s_session.session_id);
    return ESP_OK;
}

esp_err_t session_sm_set_safety_state(roast_safety_state_t state)
{
    s_session.safety_state = state;
    return ESP_OK;
}

const roast_session_t *session_sm_get_state(void)
{
    /* elapsed_ms excludes time spent paused (FR-001): frozen at the moment a
     * pause began, resuming from where it left off once resumed. Once the
     * session reaches a terminal phase (COMPLETED/ABORTED), elapsed_ms also
     * freezes at s_ended_at_ms instead of continuing to grow against live
     * wall-clock time. */
    if (s_session.phase != ROAST_PHASE_IDLE && s_session.started_at_ms > 0) {
        int64_t reference_ms;
        if (s_session.phase == ROAST_PHASE_COMPLETED || s_session.phase == ROAST_PHASE_ABORTED) {
            reference_ms = s_ended_at_ms;
        } else if (s_session.paused) {
            reference_ms = s_pause_started_at_ms;
        } else {
            reference_ms = now_ms();
        }
        s_session.elapsed_ms = reference_ms - s_session.started_at_ms - s_total_paused_ms;
    }
    return &s_session;
}

bool session_sm_has_pending_recovery(session_snapshot_t *out)
{
    session_snapshot_t snap;
    size_t len = sizeof(snap);
    if (nvs_store_get_blob(SESSION_SNAPSHOT_NVS_KEY, &snap, &len) != ESP_OK || len != sizeof(snap)) {
        return false;
    }
    if (snap.phase == ROAST_PHASE_IDLE || snap.phase == ROAST_PHASE_COMPLETED || snap.phase == ROAST_PHASE_ABORTED) {
        return false; /* Nothing active to resume. */
    }
    if (out != NULL) {
        *out = snap;
    }
    return true;
}

esp_err_t session_sm_resume_from_recovery(const session_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_session, 0, sizeof(s_session));
    strncpy(s_session.session_id, snapshot->session_id, sizeof(s_session.session_id) - 1);
    s_session.phase = snapshot->phase;
    s_session.control_mode = snapshot->control_mode;
    s_session.paused = snapshot->paused;
    s_session.mode_switch_used = snapshot->mode_switch_used;
    s_session.safety_state = ROAST_SAFETY_OK;

    int64_t n = now_ms();
    /* Continue the elapsed-time counter from where it left off - the outage
     * itself is not counted as roast time (FR-022 resumes "from the last
     * known state", not by fast-forwarding through the blackout). */
    s_session.started_at_ms = n - snapshot->elapsed_ms;
    s_session.elapsed_ms = snapshot->elapsed_ms;
    s_pause_started_at_ms = s_session.paused ? n : 0;
    s_total_paused_ms = 0;
    s_ended_at_ms = 0;

    ESP_LOGW(TAG, "Session '%s' RESUMED after power loss (phase=%d, elapsed=%lldms)",
             s_session.session_id, (int)s_session.phase, (long long)s_session.elapsed_ms);
    persist_snapshot();
    return ESP_OK;
}

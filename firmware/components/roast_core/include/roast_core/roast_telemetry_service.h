/**
 * @file roast_telemetry_service.h
 * @brief Always-on background sampling of BT/RoR/DTR/fan/heater telemetry.
 *
 * Historically this sampling loop lived inside the roast dashboard screen's
 * own LVGL timer, which meant switching away from the dashboard tab (e.g. to
 * the History tab in the new nav_shell UI) silently paused RoR feeding and
 * session history recording. This service decouples sampling/recording from
 * whichever screen happens to be visible: it runs on its own esp_timer
 * (no LVGL dependency, no LVGL lock needed) starting at boot, and caches the
 * latest values in a small snapshot struct that any UI screen can poll
 * cheaply (no sensor I/O, no file I/O) whenever it needs to repaint labels
 * or a chart.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "roast_core/session_state_machine.h"
#include "storage/session_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool sensor_valid;
    float bean_temp_c;
    float ror_c_per_min;
    float dtr_pct;       /* -1.0f if not available yet (first crack not marked). */
    int fan_pct;
    int heater_pct;
    roast_phase_t phase;
    bool paused;
    int64_t elapsed_ms;
} roast_telemetry_snapshot_t;

/** Starts the background sampling timer (500ms period). Call once at boot,
 * after max6675_init()/fan_pwm_init()/ssr_heater_init()/session_state_machine_init(). */
esp_err_t roast_telemetry_service_init(void);

/** Copies the latest cached snapshot. Cheap, safe to call every UI refresh tick. */
void roast_telemetry_service_get_snapshot(roast_telemetry_snapshot_t *out);

/** Call right after a new roast session is successfully started
 * (session_sm_start() returned ESP_OK): resets the RoR/DTR calculators and
 * opens a new session-history recording file (storage/session_store.h). */
void roast_telemetry_service_on_roast_started(void);

/** Marks the FIRST_CRACK_START event at the current elapsed roast time, so
 * DTR% starts being computed (FR-037). */
void roast_telemetry_service_mark_first_crack(void);

/** T056: appends a roast-event marker line (`{"t":<elapsed_ms>,"ev":<event_type>}`)
 * to the currently-recording session file, if any - a no-op if no session is
 * being recorded. Used by roast_core/roast_events.h so session history replay
 * (session_review.c) can reconstruct the same markers shown live. */
void roast_telemetry_service_record_event(int64_t elapsed_ms, int event_type);

/** T057: sets the BatchRecord (coffee name/origin/weight/notes) to be
 * captured into the NEXT roast's session_meta_t by
 * roast_telemetry_service_on_roast_started() - set any time before Start
 * Roast (roast_dashboard.c's Batch Info form). Purely informational. */
void roast_telemetry_service_set_pending_batch_info(const batch_record_t *batch);

/** Returns whatever BatchRecord is currently pending (possibly all-empty if never set) - used to pre-fill the Batch Info form. */
void roast_telemetry_service_get_pending_batch_info(batch_record_t *out_batch);

/**
 * Copies the session id currently being recorded into `out` (truncated to
 * `out_len`) and returns true, or returns false (leaving `out` untouched)
 * if no roast is currently being recorded. Used by the web dashboard
 * (web_api/dashboard_routes.c) to fetch and backfill the ALREADY-recorded
 * portion of the active roast's chart when a client opens/refreshes the
 * page mid-roast - without this, a fresh page load only sees telemetry
 * broadcast AFTER it connected, discarding everything recorded so far.
 */
bool roast_telemetry_service_get_recording_session_id(char *out, size_t out_len);

/** One coarse (LIVE_CHART_SAMPLE_PERIOD_MS-spaced) point kept for the web
 * dashboard's mid-roast chart backfill - see
 * roast_telemetry_service_get_live_chart_points(). */
typedef struct {
    int64_t elapsed_ms;
    float bean_temp_c;
} live_chart_point_t;

#define LIVE_CHART_MAX_POINTS 200

/**
 * Copies up to `max` already-recorded live-chart points (elapsed-time
 * order) into `out`, returning the number written. These are maintained
 * ENTIRELY IN RAM (one appended roughly every 10s during an active roast,
 * reset at roast start) - NOT read from the session's .jsonl file. This
 * replaced an earlier design that re-read/re-parsed the whole (possibly
 * multi-thousand-line) telemetry file on every web page load/refresh,
 * which was slow and, on a long enough roast, could stall the single
 * httpd worker task badly enough that the page stopped responding
 * altogether. A bounded in-RAM array is fast, lock-cheap, and can never
 * hang regardless of roast length.
 */
size_t roast_telemetry_service_get_live_chart_points(live_chart_point_t *out, size_t max);

#ifdef __cplusplus
}
#endif

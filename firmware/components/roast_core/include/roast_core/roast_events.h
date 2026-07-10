/**
 * @file roast_events.h
 * @brief T056: RoastEvent milestone marking (first/second crack manually;
 *        Turning Point and Cool Start automatically).
 *
 * CHARGE itself is already handled by session_sm_confirm_charge() (T036,
 * resets the elapsed-time reference) - it is NOT duplicated here. Per
 * operator feedback (this is a popcorn-popper-style roaster with no
 * separate "unload/drop" step - cooling starts immediately via the fan,
 * so "Drop" and "Cool Start" are the same moment), and since a manual
 * on-screen keyboard isn't practical for freeform notes, this module only
 * covers what's left as genuinely useful/practical on this hardware:
 *   - ROAST_EVENT_TURNING_POINT: detected AUTOMATICALLY by
 *     roast_telemetry_service.c (lowest BT right after CHARGE, before it
 *     starts climbing again) - no manual button.
 *   - ROAST_EVENT_FC_START / FC_END / SC_START / SC_END: marked manually
 *     by the operator (roast_dashboard.c's "Mark Event" overlay) - these
 *     are audible/sensory cues no sensor on this hardware can detect.
 *   - ROAST_EVENT_COOL_START: marked AUTOMATICALLY by
 *     profile_curve_follower.c the moment the profile's own trailing
 *     Cooling segment is reached (T038) - also represents "Drop" for this
 *     hardware, so it isn't duplicated as a separate manual event either.
 *
 * All of these are still shown as vertical reference lines on both the
 * live dashboard chart (roast_dashboard.c) and the session history detail
 * chart (session_review.c, replayed from the session's own recorded
 * .jsonl file - see roast_telemetry_service_record_event()).
 *
 * FC_START (first crack) additionally still feeds
 * roast_telemetry_service_mark_first_crack() so DTR% keeps working exactly
 * as before - this module doesn't replace that, it just also remembers the
 * mark for chart rendering and persists it into the session recording.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROAST_EVENT_TURNING_POINT = 0,
    ROAST_EVENT_FC_START,
    ROAST_EVENT_FC_END,
    ROAST_EVENT_SC_START,
    ROAST_EVENT_SC_END,
    ROAST_EVENT_COOL_START,
    ROAST_EVENT_TYPE_COUNT,
} roast_event_type_t;

#define ROAST_EVENTS_MAX 16

typedef struct {
    roast_event_type_t type;
    int64_t elapsed_ms;
} roast_event_record_t;

/** Clears the in-memory marker list for the CURRENT session. Call once when a new roast starts. */
void roast_events_reset(void);

/**
 * Records `type` at the session's CURRENT elapsed time: adds it to the
 * in-memory list (for the live dashboard), persists a marker line into the
 * active session recording (for session history replay), and - only for
 * ROAST_EVENT_FC_START - also calls roast_telemetry_service_mark_first_crack()
 * so DTR% starts being computed, same as before this module existed.
 */
esp_err_t roast_events_mark(roast_event_type_t type);

/**
 * Same as roast_events_mark(), but records `type` at an explicit
 * `elapsed_ms` rather than "now" - used by automatic detection (e.g.
 * Turning Point) where the moment being marked was actually identified a
 * few sample ticks AFTER it happened (need to observe a sustained
 * temperature rise before committing to where the minimum was).
 */
esp_err_t roast_events_mark_at(roast_event_type_t type, int64_t elapsed_ms);

/** Copies the current session's marked events (elapsed-time order) into `out`. Returns the number written. */
size_t roast_events_get_all(roast_event_record_t *out, size_t max);

/**
 * Cheap count-only query (no copying) - used to detect newly-marked events
 * (e.g. Turning Point / Cool Start, both auto-detected by OTHER modules -
 * roast_telemetry_service.c / profile_curve_follower.c - outside of any
 * user-tap-driven code path in roast_dashboard.c) without needing to poll
 * the full list every UI refresh tick.
 */
size_t roast_events_get_count(void);

/** Short 2-4 letter chart label, e.g. "TP", "FCs", "FCe", "SCs", "SCe", "Cool". */
const char *roast_event_short_label(roast_event_type_t type);

/** Full human-readable label, e.g. "Turning Point", "First Crack Start". */
const char *roast_event_full_label(roast_event_type_t type);

#ifdef __cplusplus
}
#endif

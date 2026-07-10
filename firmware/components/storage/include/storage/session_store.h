/**
 * @file session_store.h
 * @brief LittleFS-backed roast session history storage with circular retention.
 *
 * FR-024: keeps a fixed, configurable number of the most recent completed
 * sessions (default 30, see specs/001-pop-roaster-control/data-model.md);
 * once the limit is reached, the oldest session is discarded automatically.
 * Each session file also embeds its own profile snapshot (FR-034), so
 * deleting the original RoastProfile never affects stored history.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "roast_core/roast_profile.h"

#define SESSION_STORE_DEFAULT_RETENTION 30
#define SESSION_STORE_ID_MAX_LEN 40

#define SESSION_BATCH_NAME_MAX_LEN 32
#define SESSION_BATCH_NOTES_MAX_LEN 128

/** T057: BatchRecord metadata - coffee name/origin/weight/notes for this
 * specific roast, set via the display's "Batch Info" form (roast_dashboard.c)
 * before Start Roast (or any time before it, applied at the next roast
 * start). Purely informational, not used by any control logic. */
typedef struct {
    char coffee_name[SESSION_BATCH_NAME_MAX_LEN];
    char origin[SESSION_BATCH_NAME_MAX_LEN];
    float weight_g;
    char notes[SESSION_BATCH_NOTES_MAX_LEN];
} batch_record_t;

/** Per-session metadata: a human-friendly sequential roast number (since
 * there's no RTC/NTP - see spec Assumptions) plus a full snapshot of
 * whichever profile was selected when the roast started (FR-034: deleting
 * the original profile later must never affect stored history), plus the
 * BatchRecord (T057) captured at the same moment.
 *
 * NOTE: this struct grew a `batch` field after `has_profile`/`profile` were
 * already shipped - session_store_load_meta()'s fread() reads exactly
 * sizeof(session_meta_t) bytes, so a `.meta` file written by an OLDER
 * firmware build (without `batch`) is simply too short to satisfy a full
 * read and load_meta() already treats that as ESP_ERR_NOT_FOUND (same
 * graceful degradation as sessions that never had a .meta file at all) -
 * no explicit versioning/migration needed. */
typedef struct {
    uint32_t roast_number;
    bool has_profile;
    roast_profile_t profile;
    batch_record_t batch;
} session_meta_t;

esp_err_t session_store_init(void);

/** Starts a new session file; returns a generated session id via out_session_id (37+ bytes for UUID). */
esp_err_t session_store_begin_session(char *out_session_id, size_t out_session_id_size);

/** Appends a raw JSON-lines telemetry/event record to the currently active session file. */
esp_err_t session_store_append_record(const char *session_id, const char *json_line);

/** Returns the next sequential roast number (persisted in NVS, incremented every call) - simple, no-RTC-needed session naming per operator preference. */
esp_err_t session_store_next_roast_number(uint32_t *out_number);

/** Saves this session's metadata (roast number + profile snapshot, if any). Call once right after session_store_begin_session(). */
esp_err_t session_store_save_meta(const char *session_id, const session_meta_t *meta);

/** Loads a session's metadata. Returns ESP_ERR_NOT_FOUND if none was ever saved (e.g. sessions recorded before this feature existed). */
esp_err_t session_store_load_meta(const char *session_id, session_meta_t *out_meta);

/** Finalizes a session; if the retention limit is exceeded, deletes the oldest session file. */
esp_err_t session_store_finalize_session(const char *session_id);

/** Returns the number of sessions currently stored. */
esp_err_t session_store_get_count(uint32_t *out_count);

/** Gets/sets the configured retention limit (default SESSION_STORE_DEFAULT_RETENTION). */
esp_err_t session_store_set_retention_limit(uint32_t limit);
esp_err_t session_store_get_retention_limit(uint32_t *out_limit);

/**
 * Lists stored session IDs (newest-first, best-effort: filenames are
 * boot-relative-timestamp-prefixed, so ordering is only reliable within the
 * same boot cycle - see FR "no RTC" assumption). Writes up to max_ids
 * entries into out_ids; returns the number actually written via out_count.
 */
esp_err_t session_store_list_sessions(char out_ids[][SESSION_STORE_ID_MAX_LEN], size_t max_ids, size_t *out_count);

/** Opens a stored session's JSON-lines file for sequential reading (fgets/fclose). Returns NULL if not found. */
FILE *session_store_open_session(const char *session_id);

/** Deletes every stored session (both the .jsonl telemetry file and its .meta companion, if any) - used by the "Delete All" button in Roast History. Does not touch the roast counter (storage/session_store.c's ROAST_COUNTER_NVS_KEY), so future roasts keep incrementing from where they left off rather than restarting at #1. */
esp_err_t session_store_delete_all(void);

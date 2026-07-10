/**
 * @file session_store.c
 * @brief LittleFS-backed roast session history storage implementation.
 *
 * Each session is stored as a single JSON-lines file under
 * `/storage/sessions/<session_id>.jsonl` on the "storage" SPIFFS/LittleFS
 * partition (see firmware/partitions.csv). Files are named with a monotonic
 * boot-relative prefix so the oldest can be identified by directory listing
 * without needing a separate index (kept intentionally simple for an
 * embedded target with limited RAM).
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"

#include "storage/session_store.h"
#include "storage/nvs_store.h"

static const char *TAG = "session_store";
#define SESSIONS_MOUNT_POINT "/storage"
#define SESSIONS_DIR SESSIONS_MOUNT_POINT "/sessions"
#define STORAGE_PARTITION_LABEL "storage"
#define RETENTION_NVS_KEY "session_retention"
#define ROAST_COUNTER_NVS_KEY "roast_ctr"
/* Keep at least this much of the LittleFS "storage" partition free at all
 * times - proactively deletes older sessions (oldest-first) before it's
 * ever this tight, instead of only reacting to a hard "no space left"
 * write failure (which used to happen: 30 sessions by COUNT was not a safe
 * limit, since a single ~9min roast recording can be ~75KB and the whole
 * partition is only 768KB - the count-only limit let the partition fill up
 * completely before any cleanup ran). 128KB leaves headroom for a good
 * multi-minute roast recording even right after cleanup runs. */
#define SESSION_STORE_MIN_FREE_BYTES (128 * 1024)

static uint32_t s_retention_limit = SESSION_STORE_DEFAULT_RETENTION;

static void enforce_storage_limits(void);

esp_err_t session_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = SESSIONS_MOUNT_POINT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    struct stat st;
    if (stat(SESSIONS_DIR, &st) != 0) {
        mkdir(SESSIONS_DIR, 0755);
    }

    int32_t stored_limit = 0;
    if (nvs_store_get_i32(RETENTION_NVS_KEY, &stored_limit) == ESP_OK && stored_limit > 0) {
        s_retention_limit = (uint32_t)stored_limit;
    }

    ESP_LOGI(TAG, "Session store init OK (retention=%" PRIu32 ")", s_retention_limit);
    return ESP_OK;
}

esp_err_t session_store_begin_session(char *out_session_id, size_t out_session_id_size)
{
    if (out_session_id == NULL || out_session_id_size < 24) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Proactively free up space BEFORE attempting to create the new file -
     * reacting only AFTER a write failure (the old behavior) was too late,
     * since by then the partition was already completely full. */
    enforce_storage_limits();

    uint32_t rnd = esp_random();
    int64_t now_ms = esp_timer_get_time() / 1000;
    /* Zero-pad the timestamp to a fixed width so plain lexicographic string
     * comparison (sort_names_desc() below) always matches chronological
     * order - an unpadded decimal number sorts incorrectly as soon as the
     * digit count changes (e.g. "999" > "1000" as strings), which was
     * causing Roast History to appear in a seemingly random order. */
    snprintf(out_session_id, out_session_id_size, "session_%016lld_%08" PRIx32, (long long)now_ms, rnd);

    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.jsonl", out_session_id);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to create session file '%s'", path);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

esp_err_t session_store_append_record(const char *session_id, const char *json_line)
{
    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.jsonl", session_id);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", json_line);
    fclose(f);
    return ESP_OK;
}

esp_err_t session_store_next_roast_number(uint32_t *out_number)
{
    if (out_number == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int32_t current = 0;
    nvs_store_get_i32(ROAST_COUNTER_NVS_KEY, &current); /* Defaults to 0 if never set. */
    current += 1;
    esp_err_t err = nvs_store_set_i32(ROAST_COUNTER_NVS_KEY, current);
    if (err == ESP_OK) {
        *out_number = (uint32_t)current;
    }
    return err;
}

esp_err_t session_store_save_meta(const char *session_id, const session_meta_t *meta)
{
    if (session_id == NULL || meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.meta", session_id);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    size_t written = fwrite(meta, sizeof(*meta), 1, f);
    fclose(f);
    return (written == 1) ? ESP_OK : ESP_FAIL;
}

esp_err_t session_store_load_meta(const char *session_id, session_meta_t *out_meta)
{
    if (session_id == NULL || out_meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_meta, 0, sizeof(*out_meta));
    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.meta", session_id);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(out_meta, sizeof(*out_meta), 1, f);
    fclose(f);
    return (n == 1) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Finds the oldest session id (by the same zero-padded-timestamp filename
 * convention used everywhere else - lexicographic order == chronological
 * order) among the .jsonl files actually present. Returns false if there
 * are none left to delete. */
static bool find_oldest_session_id(char *out_id, size_t out_len)
{
    DIR *dir = opendir(SESSIONS_DIR);
    if (dir == NULL) {
        return false;
    }
    char oldest[128] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len < 6 || strcmp(entry->d_name + name_len - 6, ".jsonl") != 0) {
            continue; /* Only consider actual session data files, not .meta companions. */
        }
        if (oldest[0] == '\0' || strcmp(entry->d_name, oldest) < 0) {
            strncpy(oldest, entry->d_name, sizeof(oldest) - 1);
        }
    }
    closedir(dir);
    if (oldest[0] == '\0') {
        return false;
    }
    char *dot = strstr(oldest, ".jsonl");
    if (dot != NULL) {
        *dot = '\0';
    }
    strncpy(out_id, oldest, out_len - 1);
    out_id[out_len - 1] = '\0';
    return true;
}

static uint32_t count_sessions(void)
{
    DIR *dir = opendir(SESSIONS_DIR);
    if (dir == NULL) {
        return 0;
    }
    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len >= 6 && strcmp(entry->d_name + name_len - 6, ".jsonl") == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* Deletes BOTH files (.jsonl + its .meta companion, if any) for a session id
 * - deleting only the .jsonl (the old bug) would leave an orphaned .meta
 * file behind forever. remove() on a nonexistent .meta is a harmless no-op. */
static void delete_session_files(const char *session_id)
{
    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.jsonl", session_id);
    remove(path);
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.meta", session_id);
    remove(path);
}

/* FR-024: circular retention, discard oldest first - but by TWO independent
 * bounds now, not just a session count: (1) the configured retention count
 * (s_retention_limit, default 30), and (2) keeping at least
 * SESSION_STORE_MIN_FREE_BYTES free on the LittleFS "storage" partition.
 * The count alone was not a safe bound: a single ~9min roast recording can
 * be ~75KB and the whole partition is only 768KB, so 30 sessions could
 * (and did) fill it completely before the count limit was ever reached.
 * Called BOTH proactively (before creating a new session file) and after
 * finalizing one, so the partition never actually runs out of space. */
static void enforce_storage_limits(void)
{
    for (int guard = 0; guard < 64; guard++) { /* Bounded loop - never spin forever. */
        uint32_t count = count_sessions();
        size_t total_bytes = 0, used_bytes = 0;
        bool have_free_info = (esp_littlefs_info(STORAGE_PARTITION_LABEL, &total_bytes, &used_bytes) == ESP_OK);
        size_t free_bytes = (have_free_info && total_bytes > used_bytes) ? (total_bytes - used_bytes) : SIZE_MAX;

        bool over_count = count > s_retention_limit;
        bool low_space = have_free_info && free_bytes < SESSION_STORE_MIN_FREE_BYTES;
        if (!over_count && !low_space) {
            break;
        }

        char oldest_id[SESSION_STORE_ID_MAX_LEN];
        if (!find_oldest_session_id(oldest_id, sizeof(oldest_id))) {
            break; /* Nothing left to delete. */
        }

        ESP_LOGI(TAG, "Deleting oldest session '%s' (%s, %u free bytes)", oldest_id,
                 over_count ? "over retention count" : "low free space", (unsigned int)free_bytes);
        delete_session_files(oldest_id);
    }
}

esp_err_t session_store_finalize_session(const char *session_id)
{
    (void)session_id;
    enforce_storage_limits();
    return ESP_OK;
}

esp_err_t session_store_get_count(uint32_t *out_count)
{
    if (out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(SESSIONS_DIR);
    if (dir == NULL) {
        *out_count = 0;
        return ESP_OK;
    }
    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(dir);
    *out_count = count;
    return ESP_OK;
}

esp_err_t session_store_set_retention_limit(uint32_t limit)
{
    if (limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_retention_limit = limit;
    return nvs_store_set_i32(RETENTION_NVS_KEY, (int32_t)limit);
}

esp_err_t session_store_get_retention_limit(uint32_t *out_limit)
{
    if (out_limit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_limit = s_retention_limit;
    return ESP_OK;
}

/* Session filenames are prefixed with a boot-relative (esp_timer, no RTC/
 * NTP) timestamp, so plain lexicographic comparison ONLY reflects true
 * chronological order within a single boot cycle - across a reboot the
 * timer resets near zero, so a session recorded right after a reboot can
 * sort as "older" than one from a long-running previous boot, even though
 * it's actually newer (this was the cause of History appearing in a
 * seemingly random order, e.g. "#25, #27, #26"). roast_number (an NVS-
 * persisted counter that survives reboots, see session_store_next_roast_number())
 * is the reliable source of truth for creation order instead - sessions
 * with saved metadata (every session recorded from now on) are sorted by
 * roast_number descending; legacy sessions recorded before the metadata
 * feature existed (no .meta file) fall back to the old filename compare,
 * and always sort AFTER (older than) any session that does have metadata. */
typedef struct {
    char filename[256]; /* e.g. "session_xxx.jsonl" */
    uint32_t roast_number;
    bool has_meta;
} sort_entry_t;

static bool sorts_before(const sort_entry_t *a, const sort_entry_t *b)
{
    if (a->has_meta && b->has_meta) {
        return a->roast_number > b->roast_number;
    }
    if (a->has_meta != b->has_meta) {
        return a->has_meta;
    }
    return strcmp(a->filename, b->filename) > 0;
}

/* Simple insertion sort of a small array of session entries, newest-first -
 * session counts are small (retention default 30), so O(n^2) is fine here. */
static void sort_entries_desc(sort_entry_t entries[], size_t count)
{
    for (size_t i = 1; i < count; i++) {
        sort_entry_t tmp = entries[i];
        size_t j = i;
        while (j > 0 && sorts_before(&tmp, &entries[j - 1])) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }
}

esp_err_t session_store_list_sessions(char out_ids[][SESSION_STORE_ID_MAX_LEN], size_t max_ids, size_t *out_count)
{
    if (out_ids == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    DIR *dir = opendir(SESSIONS_DIR);
    if (dir == NULL) {
        return ESP_OK; /* No sessions yet - not an error. */
    }

    /* Bounded local buffer: cap at SESSION_STORE_DEFAULT_RETENTION*2 entries
     * to avoid unbounded stack usage even if more files exist on disk. */
    #define MAX_SORT_ENTRIES (SESSION_STORE_DEFAULT_RETENTION * 2)
    static sort_entry_t entries[MAX_SORT_ENTRIES];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_SORT_ENTRIES) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len < 6 || strcmp(entry->d_name + name_len - 6, ".jsonl") != 0) {
            continue; /* Skip companion files (e.g. "<id>.meta") - only list actual session data files. */
        }
        strncpy(entries[count].filename, entry->d_name, sizeof(entries[count].filename) - 1);
        entries[count].filename[sizeof(entries[count].filename) - 1] = '\0';

        char id[SESSION_STORE_ID_MAX_LEN];
        strncpy(id, entry->d_name, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        char *dot = strstr(id, ".jsonl");
        if (dot != NULL) {
            *dot = '\0';
        }
        session_meta_t meta;
        if (session_store_load_meta(id, &meta) == ESP_OK) {
            entries[count].roast_number = meta.roast_number;
            entries[count].has_meta = true;
        } else {
            entries[count].roast_number = 0;
            entries[count].has_meta = false;
        }
        count++;
    }
    closedir(dir);

    sort_entries_desc(entries, count);

    size_t written = 0;
    for (size_t i = 0; i < count && written < max_ids; i++) {
        char *dot = strstr(entries[i].filename, ".jsonl");
        if (dot != NULL) {
            *dot = '\0';
        }
        strncpy(out_ids[written], entries[i].filename, SESSION_STORE_ID_MAX_LEN - 1);
        out_ids[written][SESSION_STORE_ID_MAX_LEN - 1] = '\0';
        written++;
    }
    *out_count = written;
    return ESP_OK;
    #undef MAX_SORT_ENTRIES
}

FILE *session_store_open_session(const char *session_id)
{
    if (session_id == NULL) {
        return NULL;
    }
    char path[160];
    snprintf(path, sizeof(path), SESSIONS_DIR "/%s.jsonl", session_id);
    return fopen(path, "r");
}

esp_err_t session_store_delete_all(void)
{
    static char ids[SESSION_STORE_DEFAULT_RETENTION * 2][SESSION_STORE_ID_MAX_LEN];
    size_t count = 0;
    session_store_list_sessions(ids, SESSION_STORE_DEFAULT_RETENTION * 2, &count);
    for (size_t i = 0; i < count; i++) {
        delete_session_files(ids[i]);
    }
    ESP_LOGI(TAG, "Deleted all %d stored session(s)", (int)count);
    return ESP_OK;
}

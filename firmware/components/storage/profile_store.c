/**
 * @file profile_store.c
 * @brief See header.
 */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"

#include "storage/nvs_store.h"
#include "storage/profile_store.h"
#include "roast_core/session_state_machine.h"

static const char *TAG = "profile_store";

#define NVS_KEY_INDEX "prof_idx"
#define NVS_KEY_SELECTED "prof_sel"

typedef struct {
    uint8_t count;
    profile_store_entry_t entries[PROFILE_STORE_MAX_PROFILES];
} profile_index_t;

static void data_key(int id, char *out, size_t out_len)
{
    snprintf(out, out_len, "prof_d%d", id);
}

static esp_err_t load_index(profile_index_t *out)
{
    size_t len = sizeof(*out);
    esp_err_t err = nvs_store_get_blob(NVS_KEY_INDEX, out, &len);
    if (err != ESP_OK || len != sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t save_index(const profile_index_t *idx)
{
    return nvs_store_set_blob(NVS_KEY_INDEX, idx, sizeof(*idx));
}

static esp_err_t save_profile(int id, const roast_profile_t *profile)
{
    char key[16];
    data_key(id, key, sizeof(key));
    return nvs_store_set_blob(key, profile, sizeof(*profile));
}

static void seed_demo_profiles(void)
{
    profile_index_t idx = {0};

    roast_profile_t light = {0};
    strncpy(light.name, "Light Roast", sizeof(light.name) - 1);
    light.point_count = 5;
    light.points[0] = (roast_profile_point_t){ .duration_s = 60,  .target_temp_c = 100.0f, .target_fan_pct = 80 };
    light.points[1] = (roast_profile_point_t){ .duration_s = 180, .target_temp_c = 160.0f, .target_fan_pct = 60 };
    light.points[2] = (roast_profile_point_t){ .duration_s = 240, .target_temp_c = 195.0f, .target_fan_pct = 50 };
    light.points[3] = (roast_profile_point_t){ .duration_s = 60,  .target_temp_c = 200.0f, .target_fan_pct = 40 };
    light.points[4] = (roast_profile_point_t){ .duration_s = 240, .target_temp_c = ROAST_PROFILE_COOLING_TEMP_C, .target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT, .is_cooling = true };

    roast_profile_t city = {0};
    strncpy(city.name, "City Roast", sizeof(city.name) - 1);
    city.point_count = 5;
    city.points[0] = (roast_profile_point_t){ .duration_s = 60,  .target_temp_c = 100.0f, .target_fan_pct = 80 };
    city.points[1] = (roast_profile_point_t){ .duration_s = 210, .target_temp_c = 165.0f, .target_fan_pct = 60 };
    city.points[2] = (roast_profile_point_t){ .duration_s = 270, .target_temp_c = 205.0f, .target_fan_pct = 45 };
    city.points[3] = (roast_profile_point_t){ .duration_s = 90,  .target_temp_c = 212.0f, .target_fan_pct = 40 };
    city.points[4] = (roast_profile_point_t){ .duration_s = 240, .target_temp_c = ROAST_PROFILE_COOLING_TEMP_C, .target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT, .is_cooling = true };

    roast_profile_t full_city_plus = {0};
    strncpy(full_city_plus.name, "Full City+", sizeof(full_city_plus.name) - 1);
    full_city_plus.point_count = 6;
    full_city_plus.points[0] = (roast_profile_point_t){ .duration_s = 60,  .target_temp_c = 100.0f, .target_fan_pct = 80 };
    full_city_plus.points[1] = (roast_profile_point_t){ .duration_s = 210, .target_temp_c = 165.0f, .target_fan_pct = 60 };
    full_city_plus.points[2] = (roast_profile_point_t){ .duration_s = 270, .target_temp_c = 208.0f, .target_fan_pct = 45 };
    full_city_plus.points[3] = (roast_profile_point_t){ .duration_s = 90,  .target_temp_c = 218.0f, .target_fan_pct = 35 };
    full_city_plus.points[4] = (roast_profile_point_t){ .duration_s = 30,  .target_temp_c = 221.0f, .target_fan_pct = 35 };
    full_city_plus.points[5] = (roast_profile_point_t){ .duration_s = 240, .target_temp_c = ROAST_PROFILE_COOLING_TEMP_C, .target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT, .is_cooling = true };

    const roast_profile_t *demo_profiles[] = { &light, &city, &full_city_plus };
    idx.count = sizeof(demo_profiles) / sizeof(demo_profiles[0]);
    for (uint8_t i = 0; i < idx.count; i++) {
        idx.entries[i].id = i;
        strncpy(idx.entries[i].name, demo_profiles[i]->name, sizeof(idx.entries[i].name) - 1);
        save_profile(i, demo_profiles[i]);
    }
    save_index(&idx);
    nvs_store_set_i32(NVS_KEY_SELECTED, 0);
    ESP_LOGI(TAG, "Seeded %d demo profiles", (int)idx.count);
}

esp_err_t profile_store_init(void)
{
    profile_index_t idx;
    bool needs_seed = (load_index(&idx) != ESP_OK || idx.count == 0);

    /* Detect an on-disk roast_profile_t from an older firmware build with a
     * different struct layout/size (e.g. T034 added target_heater_pct) -
     * profile_store_load() would otherwise keep rejecting every stored
     * profile as "corrupted" (size mismatch) forever. Since this is demo
     * seed data, the simplest safe recovery is to just reseed. */
    if (!needs_seed) {
        char key[16];
        data_key(idx.entries[0].id, key, sizeof(key));
        roast_profile_t probe;
        size_t len = sizeof(probe);
        if (nvs_store_get_blob(key, &probe, &len) != ESP_OK || len != sizeof(probe)) {
            ESP_LOGW(TAG, "Stored profile data doesn't match current roast_profile_t size - reseeding demo profiles");
            needs_seed = true;
        }
    }

    if (needs_seed) {
        seed_demo_profiles();
    }
    ESP_LOGI(TAG, "Profile store init OK");
    return ESP_OK;
}

esp_err_t profile_store_list(profile_store_entry_t *out_entries, size_t max, size_t *out_count)
{
    profile_index_t idx;
    if (load_index(&idx) != ESP_OK) {
        *out_count = 0;
        return ESP_OK;
    }
    size_t n = idx.count;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out_entries[i] = idx.entries[i];
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t profile_store_load(int id, roast_profile_t *out)
{
    char key[16];
    data_key(id, key, sizeof(key));
    size_t len = sizeof(*out);
    esp_err_t err = nvs_store_get_blob(key, out, &len);
    if (err != ESP_OK || len != sizeof(*out)) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Defensive: point_count must never exceed the fixed-size points[]
     * array it indexes into. A corrupted/stale blob (e.g. from an older
     * firmware build with a different roast_profile_t layout that somehow
     * slipped past the size check above) with a garbage point_count would
     * otherwise send every consumer (profile_editor.c's segment-card loop
     * especially) spinning through hundreds of bogus "segments" - each one
     * allocating ~15 LVGL objects - which is slow enough to trip the task
     * watchdog and reboot the device. Clamp instead of trusting the stored
     * value blindly. */
    if (out->point_count > ROAST_PROFILE_MAX_POINTS) {
        ESP_LOGW(TAG, "profile_store_load(id=%d): point_count %d exceeds max %d - clamping",
                 id, out->point_count, ROAST_PROFILE_MAX_POINTS);
        out->point_count = ROAST_PROFILE_MAX_POINTS;
    }
    return ESP_OK;
}

esp_err_t profile_store_create(const roast_profile_t *profile, int *out_id)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Authoritative last line of defense: every stored profile must end
     * with exactly one Cooling segment, regardless of which caller
     * (display editor, web editor, import) is saving it. */
    roast_profile_t normalized = *profile;
    roast_profile_ensure_trailing_cooling(&normalized);
    profile = &normalized;

    profile_index_t idx;
    if (load_index(&idx) != ESP_OK) {
        memset(&idx, 0, sizeof(idx));
    }
    if (idx.count >= PROFILE_STORE_MAX_PROFILES) {
        ESP_LOGW(TAG, "profile_store_create rejected: store full (%d profiles)", PROFILE_STORE_MAX_PROFILES);
        return ESP_ERR_NO_MEM;
    }

    /* Find the first id (0..MAX-1) not already in use. */
    int id = -1;
    for (int candidate = 0; candidate < PROFILE_STORE_MAX_PROFILES; candidate++) {
        bool used = false;
        for (uint8_t i = 0; i < idx.count; i++) {
            if (idx.entries[i].id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            id = candidate;
            break;
        }
    }
    if (id < 0) {
        return ESP_ERR_NO_MEM; /* Shouldn't happen given the count check above, but stay defensive. */
    }

    esp_err_t err = save_profile(id, profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile_store_create: failed to write profile data (id=%d): %s", id, esp_err_to_name(err));
        return err;
    }

    idx.entries[idx.count].id = id;
    strncpy(idx.entries[idx.count].name, profile->name, sizeof(idx.entries[idx.count].name) - 1);
    idx.entries[idx.count].name[sizeof(idx.entries[idx.count].name) - 1] = '\0';
    idx.count++;

    err = save_index(&idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile_store_create: failed to write updated index: %s", esp_err_to_name(err));
        return err;
    }

    if (out_id != NULL) {
        *out_id = id;
    }
    ESP_LOGI(TAG, "Created profile '%s' (id=%d)", profile->name, id);
    return ESP_OK;
}

esp_err_t profile_store_update(int id, const roast_profile_t *profile)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    roast_profile_t normalized = *profile;
    roast_profile_ensure_trailing_cooling(&normalized);
    profile = &normalized;

    profile_index_t idx;
    if (load_index(&idx) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    int found = -1;
    for (uint8_t i = 0; i < idx.count; i++) {
        if (idx.entries[i].id == id) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = save_profile(id, profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile_store_update: failed to write profile data (id=%d): %s", id, esp_err_to_name(err));
        return err;
    }

    strncpy(idx.entries[found].name, profile->name, sizeof(idx.entries[found].name) - 1);
    idx.entries[found].name[sizeof(idx.entries[found].name) - 1] = '\0';
    err = save_index(&idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile_store_update: failed to write updated index: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Updated profile '%s' (id=%d)", profile->name, id);
    return ESP_OK;
}

esp_err_t profile_store_delete(int id)
{
    profile_index_t idx;
    if (load_index(&idx) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    int found = -1;
    for (uint8_t i = 0; i < idx.count; i++) {
        if (idx.entries[i].id == id) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (uint8_t i = (uint8_t)found; i < idx.count - 1; i++) {
        idx.entries[i] = idx.entries[i + 1];
    }
    idx.count--;

    esp_err_t err = save_index(&idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile_store_delete: failed to write updated index: %s", esp_err_to_name(err));
        return err;
    }

    char key[16];
    data_key(id, key, sizeof(key));
    nvs_store_erase_key(key); /* Best-effort - the index no longer references it either way. */

    /* If the deleted profile was selected, clear the selection so the Roast
     * dashboard correctly falls back to "no preset selected" instead of
     * silently referencing a now-deleted id. */
    int32_t sel;
    if (nvs_store_get_i32(NVS_KEY_SELECTED, &sel) == ESP_OK && sel == id) {
        nvs_store_erase_key(NVS_KEY_SELECTED);
    }

    ESP_LOGI(TAG, "Deleted profile id=%d", id);
    return ESP_OK;
}

esp_err_t profile_store_set_selected(int id)
{
    /* Bug found by the operator: switching the selected preset mid-roast
     * broke the whole screen - the dashboard/profile_curve_follower.c both
     * assume the selected profile stays fixed for a session's entire
     * lifetime (loaded once at session start). Refuse the switch outright
     * while a session is actively running instead of silently corrupting
     * that assumption; the operator must Cancel/finish the current roast
     * first. */
    const roast_session_t *session = session_sm_get_state();
    if (session->phase != ROAST_PHASE_IDLE && session->phase != ROAST_PHASE_COMPLETED &&
        session->phase != ROAST_PHASE_ABORTED) {
        ESP_LOGW(TAG, "Refusing to switch selected preset while a roast session is active (phase=%d)", (int)session->phase);
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_store_set_i32(NVS_KEY_SELECTED, id);
}

esp_err_t profile_store_get_selected_id(int *out_id)
{
    int32_t id;
    if (nvs_store_get_i32(NVS_KEY_SELECTED, &id) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_id = (int)id;
    return ESP_OK;
}

esp_err_t profile_store_get_selected(roast_profile_t *out)
{
    int32_t id;
    if (nvs_store_get_i32(NVS_KEY_SELECTED, &id) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return profile_store_load((int)id, out);
}

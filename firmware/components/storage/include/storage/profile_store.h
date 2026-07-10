/**
 * @file profile_store.h
 * @brief T031 (minimal): NVS-backed persistence for Roast Profiles
 *        (roast_core/roast_profile.h), plus a "currently selected profile"
 *        pointer used by the Roast dashboard. Seeds a handful of demo
 *        profiles on first boot so the Presets tab has something to select
 *        before a full profile editor (T032) exists.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "roast_core/roast_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_STORE_MAX_PROFILES 10

typedef struct {
    int id;
    char name[ROAST_PROFILE_NAME_MAX_LEN];
} profile_store_entry_t;

/** Call once at boot, after nvs_store_init(). Seeds demo profiles if none exist yet. */
esp_err_t profile_store_init(void);

/** Lists every stored profile (id + name only, not the full curve). */
esp_err_t profile_store_list(profile_store_entry_t *out_entries, size_t max, size_t *out_count);

/** Loads the full profile (including its curve points) for the given id. */
esp_err_t profile_store_load(int id, roast_profile_t *out);

/**
 * T032: creates a brand-new stored profile, assigning it the first free id
 * (0..PROFILE_STORE_MAX_PROFILES-1). Returns ESP_ERR_NO_MEM if the store is
 * already at PROFILE_STORE_MAX_PROFILES.
 */
esp_err_t profile_store_create(const roast_profile_t *profile, int *out_id);

/** T032: overwrites an existing stored profile's data (curve points + name) in place. Returns ESP_ERR_NOT_FOUND if `id` doesn't exist. */
esp_err_t profile_store_update(int id, const roast_profile_t *profile);

/** T032: deletes a stored profile. If it was the currently selected profile, the selection is cleared (profile_store_get_selected*() will then return ESP_ERR_NOT_FOUND until a new one is selected). Returns ESP_ERR_NOT_FOUND if `id` doesn't exist. */
esp_err_t profile_store_delete(int id);

/** Marks `id` as the profile the next roast should run (persisted across reboots). */
esp_err_t profile_store_set_selected(int id);

/** Returns the currently selected profile's id, or ESP_ERR_NOT_FOUND if none is selected. */
esp_err_t profile_store_get_selected_id(int *out_id);

/** Loads the currently selected profile. Returns ESP_ERR_NOT_FOUND if none is selected. */
esp_err_t profile_store_get_selected(roast_profile_t *out);

#ifdef __cplusplus
}
#endif

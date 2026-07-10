/**
 * @file i18n.h
 * @brief Runtime-switchable i18n string catalog loader (FR-038).
 *
 * English is the default language; Portuguese and Spanish are also
 * supported. The active language can be changed at any time from settings,
 * on both the display (LVGL) and the web dashboard, which share this same
 * catalog (research.md Decision 2/7).
 */
#pragma once

#include "esp_err.h"

typedef enum {
    I18N_LANG_EN = 0, /* Default. */
    I18N_LANG_PT,
    I18N_LANG_ES,
} i18n_lang_t;

/** String keys - extend as UI screens are implemented (T055 fills in the actual catalogs). */
typedef enum {
    I18N_KEY_DASHBOARD_TITLE = 0,
    I18N_KEY_START_ROAST,
    I18N_KEY_STOP_ROAST,
    I18N_KEY_PAUSE,
    I18N_KEY_RESUME,
    I18N_KEY_EMERGENCY_STOP,
    I18N_KEY_ALARM_ACKNOWLEDGE,
    I18N_KEY_NAV_ROAST,
    I18N_KEY_NAV_MANUAL,
    I18N_KEY_NAV_PRESETS,
    I18N_KEY_NAV_HISTORY,
    I18N_KEY_NAV_CONFIG,
    I18N_KEY_CONFIG_TITLE,
    I18N_KEY_PERIPHERAL_TEST,
    I18N_KEY_SENSOR_CALIBRATION,
    I18N_KEY_WIFI_SETUP,
    I18N_KEY_LANGUAGE,
    I18N_KEY_COUNT,
} i18n_key_t;

/** Loads the persisted language preference (default EN if none stored yet). */
esp_err_t i18n_init(void);

/** Changes the active language immediately; persists the choice (FR-038). */
esp_err_t i18n_set_language(i18n_lang_t lang);

i18n_lang_t i18n_get_language(void);

/** Returns the localized string for a given key in the currently active language. */
const char *i18n_get(i18n_key_t key);

/** Returns a short display name for a language ("EN"/"PT"/"ES"), for language-picker UI. */
const char *i18n_get_language_code(i18n_lang_t lang);

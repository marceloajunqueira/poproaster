/**
 * @file i18n.c
 * @brief i18n string catalog loader implementation.
 *
 * Minimal built-in catalog for the keys defined so far; T055 (Polish phase)
 * replaces/extends this with the full en.json/pt.json/es.json catalogs and
 * a proper loader (e.g. reading from the "storage" LittleFS partition)
 * instead of these inline C string tables.
 */
#include "esp_log.h"

#include "storage/nvs_store.h"
#include "ui_display/i18n.h"

static const char *TAG = "i18n";
static i18n_lang_t s_lang = I18N_LANG_EN;

static const char *const kStringsEn[I18N_KEY_COUNT] = {
    [I18N_KEY_DASHBOARD_TITLE] = "Pop Roaster",
    [I18N_KEY_START_ROAST] = "Start Roast",
    [I18N_KEY_STOP_ROAST] = "Stop Roast",
    [I18N_KEY_PAUSE] = "Pause",
    [I18N_KEY_RESUME] = "Resume",
    [I18N_KEY_EMERGENCY_STOP] = "EMERGENCY STOP",
    [I18N_KEY_ALARM_ACKNOWLEDGE] = "Acknowledge Alarm",
};

static const char *const kStringsPt[I18N_KEY_COUNT] = {
    [I18N_KEY_DASHBOARD_TITLE] = "Pop Roaster",
    [I18N_KEY_START_ROAST] = "Iniciar Torra",
    [I18N_KEY_STOP_ROAST] = "Finalizar Torra",
    [I18N_KEY_PAUSE] = "Pausar",
    [I18N_KEY_RESUME] = "Retomar",
    [I18N_KEY_EMERGENCY_STOP] = "PARADA DE EMERGENCIA",
    [I18N_KEY_ALARM_ACKNOWLEDGE] = "Reconhecer Alarme",
};

static const char *const kStringsEs[I18N_KEY_COUNT] = {
    [I18N_KEY_DASHBOARD_TITLE] = "Pop Roaster",
    [I18N_KEY_START_ROAST] = "Iniciar Tueste",
    [I18N_KEY_STOP_ROAST] = "Finalizar Tueste",
    [I18N_KEY_PAUSE] = "Pausar",
    [I18N_KEY_RESUME] = "Reanudar",
    [I18N_KEY_EMERGENCY_STOP] = "PARADA DE EMERGENCIA",
    [I18N_KEY_ALARM_ACKNOWLEDGE] = "Reconocer Alarma",
};

esp_err_t i18n_init(void)
{
    int32_t stored_lang = I18N_LANG_EN;
    if (nvs_store_get_i32("language", &stored_lang) == ESP_OK &&
        stored_lang >= I18N_LANG_EN && stored_lang <= I18N_LANG_ES) {
        s_lang = (i18n_lang_t)stored_lang;
    } else {
        s_lang = I18N_LANG_EN; /* FR-038: English is the default. */
    }
    ESP_LOGI(TAG, "i18n init OK (language=%d)", (int)s_lang);
    return ESP_OK;
}

esp_err_t i18n_set_language(i18n_lang_t lang)
{
    if (lang < I18N_LANG_EN || lang > I18N_LANG_ES) {
        return ESP_ERR_INVALID_ARG;
    }
    s_lang = lang;
    return nvs_store_set_i32("language", (int32_t)lang);
}

i18n_lang_t i18n_get_language(void)
{
    return s_lang;
}

const char *i18n_get(i18n_key_t key)
{
    if (key < 0 || key >= I18N_KEY_COUNT) {
        return "";
    }
    switch (s_lang) {
        case I18N_LANG_PT: return kStringsPt[key];
        case I18N_LANG_ES: return kStringsEs[key];
        case I18N_LANG_EN:
        default: return kStringsEn[key];
    }
}

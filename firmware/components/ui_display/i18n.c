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
    [I18N_KEY_NAV_ROAST] = "Roast",
    [I18N_KEY_NAV_MANUAL] = "Manual",
    [I18N_KEY_NAV_PRESETS] = "Presets",
    [I18N_KEY_NAV_HISTORY] = "History",
    [I18N_KEY_NAV_CONFIG] = "Config",
    [I18N_KEY_CONFIG_TITLE] = "Config",
    [I18N_KEY_PERIPHERAL_TEST] = "Peripheral Test",
    [I18N_KEY_SENSOR_CALIBRATION] = "Sensor Calibration",
    [I18N_KEY_WIFI_SETUP] = "Wi-Fi Setup",
    [I18N_KEY_LANGUAGE] = "Language",
    [I18N_KEY_BACK] = "Back",
    [I18N_KEY_PID_AUTOTUNE] = "PID Autotune",
    [I18N_KEY_PID_AUTOTUNE_NOTE] =
        "Raises the fan to 100% and cycles the heater near 150C for several minutes to find PID gains "
        "(noisy, temp will swing). Self-aborts on any issue and saves the result automatically.",
    [I18N_KEY_PID_AUTOTUNE_CONSENT] = "I understand the risks and want to start the autotune",
    [I18N_KEY_PID_AUTOTUNE_START] = "Start",
    [I18N_KEY_PID_AUTOTUNE_CANCEL] = "Cancel",
    [I18N_KEY_PID_AUTOTUNE_STATE_IDLE] = "Idle",
    [I18N_KEY_PID_AUTOTUNE_STATE_RUNNING] = "Running",
    [I18N_KEY_PID_AUTOTUNE_STATE_SUCCEEDED] = "Succeeded",
    [I18N_KEY_PID_AUTOTUNE_STATE_FAILED] = "Failed",
    [I18N_KEY_PID_AUTOTUNE_STATUS_IDLE] = "Idle - check the box below, then Start.",
    [I18N_KEY_PID_AUTOTUNE_STATUS_PREPARING] = "Preparing - raising fan to full speed...",
    [I18N_KEY_PID_AUTOTUNE_STATUS_FMT] = "%s - %us elapsed, phase %u/%u - %s",
    [I18N_KEY_PID_AUTOTUNE_START_ERROR_FMT] = "Could not start: %s",
    [I18N_KEY_PID_AUTOTUNE_RESULT_APPLIED_FMT] =
        "Applied automatically (No-Overshoot rule, saved to NVS):\nKp=%.3f Ki=%.4f Kd=%.2f",
    [I18N_KEY_PID_AUTOTUNE_RESULT_FAILED] = "No gains were applied.",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT] = "Fan: %d%%   BT: %.1fC   Heater: %d%%",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT_NO_BT] = "Fan: %d%%   BT: --   Heater: %d%%",
};

static const char *const kStringsPt[I18N_KEY_COUNT] = {
    [I18N_KEY_DASHBOARD_TITLE] = "Pop Roaster",
    [I18N_KEY_START_ROAST] = "Iniciar Torra",
    [I18N_KEY_STOP_ROAST] = "Finalizar Torra",
    [I18N_KEY_PAUSE] = "Pausar",
    [I18N_KEY_RESUME] = "Retomar",
    [I18N_KEY_EMERGENCY_STOP] = "PARADA DE EMERGENCIA",
    [I18N_KEY_ALARM_ACKNOWLEDGE] = "Reconhecer Alarme",
    [I18N_KEY_NAV_ROAST] = "Torra",
    [I18N_KEY_NAV_MANUAL] = "Manual",
    [I18N_KEY_NAV_PRESETS] = "Perfis",
    [I18N_KEY_NAV_HISTORY] = "Historico",
    [I18N_KEY_NAV_CONFIG] = "Config",
    [I18N_KEY_CONFIG_TITLE] = "Configuracoes",
    [I18N_KEY_PERIPHERAL_TEST] = "Teste de Perifericos",
    [I18N_KEY_SENSOR_CALIBRATION] = "Calibracao do Sensor",
    [I18N_KEY_WIFI_SETUP] = "Configurar Wi-Fi",
    [I18N_KEY_LANGUAGE] = "Idioma",
    [I18N_KEY_BACK] = "Voltar",
    [I18N_KEY_PID_AUTOTUNE] = "Autoajuste PID",
    [I18N_KEY_PID_AUTOTUNE_NOTE] =
        "Eleva o ventilador a 100% e liga/desliga o aquecedor perto de 150C por alguns minutos para "
        "encontrar os ganhos do PID (barulhento, a temperatura vai oscilar). Cancela sozinho se algo "
        "der errado e salva o resultado automaticamente.",
    [I18N_KEY_PID_AUTOTUNE_CONSENT] = "Estou ciente dos riscos e quero iniciar o autoajuste",
    [I18N_KEY_PID_AUTOTUNE_START] = "Iniciar",
    [I18N_KEY_PID_AUTOTUNE_CANCEL] = "Cancelar",
    [I18N_KEY_PID_AUTOTUNE_STATE_IDLE] = "Ocioso",
    [I18N_KEY_PID_AUTOTUNE_STATE_RUNNING] = "Executando",
    [I18N_KEY_PID_AUTOTUNE_STATE_SUCCEEDED] = "Concluido",
    [I18N_KEY_PID_AUTOTUNE_STATE_FAILED] = "Falhou",
    [I18N_KEY_PID_AUTOTUNE_STATUS_IDLE] = "Ocioso - marque a caixa abaixo e depois toque em Iniciar.",
    [I18N_KEY_PID_AUTOTUNE_STATUS_PREPARING] = "Preparando - elevando o ventilador a velocidade maxima...",
    [I18N_KEY_PID_AUTOTUNE_STATUS_FMT] = "%s - %us decorridos, fase %u/%u - %s",
    [I18N_KEY_PID_AUTOTUNE_START_ERROR_FMT] = "Nao foi possivel iniciar: %s",
    [I18N_KEY_PID_AUTOTUNE_RESULT_APPLIED_FMT] =
        "Aplicado automaticamente (regra Sem Overshoot, salvo na memoria):\nKp=%.3f Ki=%.4f Kd=%.2f",
    [I18N_KEY_PID_AUTOTUNE_RESULT_FAILED] = "Nenhum ganho foi aplicado.",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT] = "Fan: %d%%   BT: %.1fC   Aquecedor: %d%%",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT_NO_BT] = "Fan: %d%%   BT: --   Aquecedor: %d%%",
};

static const char *const kStringsEs[I18N_KEY_COUNT] = {
    [I18N_KEY_DASHBOARD_TITLE] = "Pop Roaster",
    [I18N_KEY_START_ROAST] = "Iniciar Tueste",
    [I18N_KEY_STOP_ROAST] = "Finalizar Tueste",
    [I18N_KEY_PAUSE] = "Pausar",
    [I18N_KEY_RESUME] = "Reanudar",
    [I18N_KEY_EMERGENCY_STOP] = "PARADA DE EMERGENCIA",
    [I18N_KEY_ALARM_ACKNOWLEDGE] = "Reconocer Alarma",
    [I18N_KEY_NAV_ROAST] = "Tueste",
    [I18N_KEY_NAV_MANUAL] = "Manual",
    [I18N_KEY_NAV_PRESETS] = "Perfiles",
    [I18N_KEY_NAV_HISTORY] = "Historial",
    [I18N_KEY_NAV_CONFIG] = "Config",
    [I18N_KEY_CONFIG_TITLE] = "Configuracion",
    [I18N_KEY_PERIPHERAL_TEST] = "Prueba de Perifericos",
    [I18N_KEY_SENSOR_CALIBRATION] = "Calibracion del Sensor",
    [I18N_KEY_WIFI_SETUP] = "Configurar Wi-Fi",
    [I18N_KEY_LANGUAGE] = "Idioma",
    [I18N_KEY_BACK] = "Volver",
    [I18N_KEY_PID_AUTOTUNE] = "Autoajuste PID",
    [I18N_KEY_PID_AUTOTUNE_NOTE] =
        "Eleva el ventilador al 100% y enciende/apaga el calentador cerca de 150C por varios minutos "
        "para encontrar las ganancias del PID (ruidoso, la temperatura oscilara). Se cancela solo si "
        "algo falla y guarda el resultado automaticamente.",
    [I18N_KEY_PID_AUTOTUNE_CONSENT] = "Soy consciente de los riesgos y quiero iniciar el autoajuste",
    [I18N_KEY_PID_AUTOTUNE_START] = "Iniciar",
    [I18N_KEY_PID_AUTOTUNE_CANCEL] = "Cancelar",
    [I18N_KEY_PID_AUTOTUNE_STATE_IDLE] = "Inactivo",
    [I18N_KEY_PID_AUTOTUNE_STATE_RUNNING] = "Ejecutando",
    [I18N_KEY_PID_AUTOTUNE_STATE_SUCCEEDED] = "Completado",
    [I18N_KEY_PID_AUTOTUNE_STATE_FAILED] = "Fallido",
    [I18N_KEY_PID_AUTOTUNE_STATUS_IDLE] = "Inactivo - marque la casilla abajo y luego toque Iniciar.",
    [I18N_KEY_PID_AUTOTUNE_STATUS_PREPARING] = "Preparando - elevando el ventilador a velocidad maxima...",
    [I18N_KEY_PID_AUTOTUNE_STATUS_FMT] = "%s - %us transcurridos, fase %u/%u - %s",
    [I18N_KEY_PID_AUTOTUNE_START_ERROR_FMT] = "No se pudo iniciar: %s",
    [I18N_KEY_PID_AUTOTUNE_RESULT_APPLIED_FMT] =
        "Aplicado automaticamente (regla Sin Sobreimpulso, guardado en memoria):\nKp=%.3f Ki=%.4f Kd=%.2f",
    [I18N_KEY_PID_AUTOTUNE_RESULT_FAILED] = "No se aplico ninguna ganancia.",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT] = "Fan: %d%%   BT: %.1fC   Calentador: %d%%",
    [I18N_KEY_PID_AUTOTUNE_LIVE_FMT_NO_BT] = "Fan: %d%%   BT: --   Calentador: %d%%",
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

const char *i18n_get_language_code(i18n_lang_t lang)
{
    switch (lang) {
        case I18N_LANG_PT: return "PT";
        case I18N_LANG_ES: return "ES";
        case I18N_LANG_EN:
        default: return "EN";
    }
}

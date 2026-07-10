/**
 * @file wifi_provisioning.c
 * @brief WiFi provisioning implementation (AP+captive portal, then STA).
 *
 * FR-023: on first use (or when no valid credentials are stored), the device
 * starts its own Access Point with a captive portal so the operator can enter
 * the local WiFi's SSID/password without any external tool. Once configured,
 * it connects as a station (STA) to the local network. FR-023 edge case:
 * on STA connect failure, re-enter the AP portal for reconfiguration.
 */
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "hal/wifi_provisioning.h"
#include "storage/nvs_store.h"

static const char *TAG = "wifi_provisioning";

#define WIFI_PROV_AP_SSID_PREFIX "PopRoaster-Setup"
#define WIFI_PROV_MAX_RETRIES 5

static wifi_prov_state_t s_state = WIFI_PROV_STATE_AP_PORTAL;
static int s_retry_count = 0;
static esp_netif_t *s_sta_netif;

static void start_ap_portal(void)
{
    ESP_LOGI(TAG, "Starting AP + captive portal (%s) for WiFi setup", WIFI_PROV_AP_SSID_PREFIX);
    s_state = WIFI_PROV_STATE_AP_PORTAL;

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, WIFI_PROV_AP_SSID_PREFIX, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; /* Setup-only network; trusted-network assumption per FR-021. */
    ap_config.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    /* NOTE: the actual DNS-hijack captive portal HTTP redirect is implemented
     * alongside the setup web page (T049, firmware/webui/src/wifi_setup.html) using
     * the HTTP server started in web_api/server.c. */
}

static void try_connect_sta(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID '%s' as STA", ssid);
    s_state = WIFI_PROV_STATE_CONNECTING;

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_PROV_MAX_RETRIES) {
            s_retry_count++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry_count, WIFI_PROV_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA connect failed after %d retries; re-entering AP portal", WIFI_PROV_MAX_RETRIES);
            s_state = WIFI_PROV_STATE_FAILED;
            start_ap_portal();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "STA connected, got IP");
        s_retry_count = 0;
        s_state = WIFI_PROV_STATE_CONNECTED;
    }
}

esp_err_t wifi_provisioning_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char stored_ssid[33] = {0};
    char stored_password[65] = {0};
    bool has_creds = (nvs_store_get_string("wifi_ssid", stored_ssid, sizeof(stored_ssid)) == ESP_OK) &&
                      strlen(stored_ssid) > 0;

    if (has_creds) {
        nvs_store_get_string("wifi_password", stored_password, sizeof(stored_password));
        ESP_ERROR_CHECK(esp_wifi_start());
        try_connect_sta(stored_ssid, stored_password);
    } else {
        ESP_ERROR_CHECK(esp_wifi_start());
        start_ap_portal();
    }

    return ESP_OK;
}

esp_err_t wifi_provisioning_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_store_set_string("wifi_ssid", ssid);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_store_set_string("wifi_password", password);
    if (err != ESP_OK) {
        return err;
    }
    s_retry_count = 0;
    try_connect_sta(ssid, password);
    return ESP_OK;
}

wifi_prov_state_t wifi_provisioning_get_state(void)
{
    return s_state;
}

esp_err_t wifi_provisioning_get_ip_str(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != WIFI_PROV_STATE_CONNECTED || s_sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

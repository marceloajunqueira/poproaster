/**
 * @file wifi_provisioning.h
 * @brief WiFi provisioning: AP + captive portal on first boot, then STA (FR-023).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    WIFI_PROV_STATE_AP_PORTAL = 0, /* No valid credentials yet; AP + captive portal active. */
    WIFI_PROV_STATE_CONNECTING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_FAILED,        /* STA connect failed; re-enters AP portal (edge case in spec.md). */
} wifi_prov_state_t;

/**
 * Initializes WiFi. If valid STA credentials are already stored (NVS), tries
 * to connect as a client; otherwise starts the AP + captive portal so the
 * operator can enter WiFi credentials without external tools.
 */
esp_err_t wifi_provisioning_init(void);

/** Called by the captive portal / setup web page (T049) to save new credentials and reconnect. */
esp_err_t wifi_provisioning_set_credentials(const char *ssid, const char *password);

/** Returns the current provisioning/connection state. */
wifi_prov_state_t wifi_provisioning_get_state(void);

/** Formats the STA's current IPv4 address (e.g. "192.168.1.42") into `out`. Returns ESP_ERR_INVALID_STATE if not currently connected (state != WIFI_PROV_STATE_CONNECTED). */
esp_err_t wifi_provisioning_get_ip_str(char *out, size_t out_len);

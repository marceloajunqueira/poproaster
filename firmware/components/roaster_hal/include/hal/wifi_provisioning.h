/**
 * @file wifi_provisioning.h
 * @brief WiFi provisioning: AP + captive portal on first boot, then STA (FR-023).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

/** mDNS hostname this device advertises once connected as STA - reachable
 * as "http://poproaster.local/" from any client whose OS/network supports
 * mDNS (mDNSResponder/Bonjour on macOS/iOS out of the box; most Linux
 * desktops via Avahi; Android/Windows usually need an mDNS-aware app/
 * browser or a small helper, but most home routers + phones handle it
 * fine in practice). Exposed as a macro so UI code (settings_hub.c,
 * web_api pages) can reference the exact same hostname without
 * duplicating the literal string. */
#define WIFI_PROV_MDNS_HOSTNAME "poproaster"

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

/** Returns the currently configured/connected SSID (whatever was last saved via wifi_provisioning_set_credentials(), even if not currently connected). Empty string if none saved yet. */
esp_err_t wifi_provisioning_get_ssid_str(char *out, size_t out_len);

/** Returns the STA network interface handle (needed by things that bind to a specific netif, e.g. the Modbus TCP slave in artisan_adapter). NULL if wifi_provisioning_init() hasn't run yet. */
esp_netif_t *wifi_provisioning_get_sta_netif(void);

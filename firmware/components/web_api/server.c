/**
 * @file server.c
 * @brief Base HTTP/WebSocket server skeleton implementation.
 *
 * No authentication is applied on any route (FR-021: v1 explicitly trusts
 * the local network; see spec.md Assumptions and the project constitution's
 * Security & Trust Boundaries section).
 */
#include "esp_log.h"

#include "web_api/server.h"
#include "web_api/wifi_setup_routes.h"
#include "web_api/dashboard_routes.h"
#include "web_api/history_routes.h"
#include "web_api/presets_routes.h"
#include "web_api/diagnostics_routes.h"
#include "web_api/ota_routes.h"

static const char *TAG = "web_api_server";
static httpd_handle_t s_server = NULL;

esp_err_t web_api_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* Default is 8 - this project now registers 18 routes across
     * wifi_setup_routes.c/dashboard_routes.c/history_routes.c/
     * presets_routes.c/diagnostics_routes.c/ota_routes.c, so the default
     * silently drops the last ones registered (httpd_register_uri_handler
     * starts failing with ESP_ERR_NO_MEM once full) unless raised. Bumped
     * to 26 for headroom beyond the current count. */
    config.max_uri_handlers = 26;
    /* Default is 7. The live dashboard now holds a WebSocket connection
     * open for as long as its browser tab stays open (/ws/telemetry), so
     * each open tab permanently ties up one socket instead of the usual
     * quick connect-request-disconnect HTTP pattern - a couple of tabs
     * left open across testing sessions could exhaust the default pool and
     * make brand-new page loads sit waiting for a free socket (perceived
     * as "the page takes forever to open"). Raised alongside
     * CONFIG_LWIP_MAX_SOCKETS (sdkconfig, bumped 10->16) to leave headroom.
     * lru_purge_enable means a new connection recycles the least-recently-
     * used existing one instead of being rejected/stalled once the pool is
     * full. */
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    /* Default is 4096 bytes. Crash fix (Guru Meditation LoadProhibited with
     * a "|<-CORRUPTED" backtrace and 0xa5a5a5a5-poisoned registers when
     * opening a Roast History detail page - the classic ESP-IDF stack-
     * overflow signature): several handlers in this project (notably
     * history_routes.c's history_detail_get_handler, which stacks a
     * `char stats_html[1536]` alongside several smaller buffers) push
     * cumulative stack usage close to the default limit once snprintf's
     * own internal call depth (float/string formatting) and the chunked
     * httpd_resp_send_chunk()/lwip send call chain are added on top.
     * Raised with headroom rather than trimming every handler individually -
     * the biggest stack buffers were ALSO converted to `static` in
     * history_routes.c as a second, independent mitigation, but a bigger
     * stack is the more robust fix against any other handler with a
     * similar (currently unnoticed) stack footprint. */
    config.stack_size = 8192;
    /* No auth handler registered anywhere - see FR-021. */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_setup_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_setup_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = dashboard_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dashboard_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = history_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = presets_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "presets_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = diagnostics_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "diagnostics_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ota_routes_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_routes_register failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Web API server started");
    return ESP_OK;
}

httpd_handle_t web_api_server_get_handle(void)
{
    return s_server;
}

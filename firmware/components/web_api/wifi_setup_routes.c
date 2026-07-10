/**
 * @file wifi_setup_routes.c
 * @brief Minimal WiFi setup web page implementation (see header).
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

#include "hal/wifi_provisioning.h"
#include "web_api/wifi_setup_routes.h"
#include "web_api/dashboard_routes.h"

static const char *TAG = "wifi_setup_routes";

#define FORM_BODY_MAX_LEN 256

/* Self-contained Material-Design-inspired dark theme (no external CDN -
 * the device must work fully offline / on its own AP, per FR-021). */
#define MATERIAL_DARK_STYLE \
    "<style>" \
    ":root{--bg:#121212;--surface:#1e1e1e;--primary:#FF9746;--on-surface:#e0e0e0;--muted:#9e9e9e;}" \
    "*{box-sizing:border-box;}" \
    "body{background:var(--bg);color:var(--on-surface);font-family:Roboto,'Segoe UI',Arial,sans-serif;" \
    "margin:0;padding:24px;display:flex;justify-content:center;}" \
    ".card{background:var(--surface);border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.6);" \
    "padding:24px;max-width:420px;width:100%;}" \
    "h1{font-size:20px;font-weight:500;margin:0 0 8px;color:var(--on-surface);}" \
    "p{color:var(--muted);font-size:14px;line-height:1.4;}" \
    "label{display:block;margin:16px 0 6px;font-size:13px;color:var(--muted);}" \
    "input{width:100%;padding:10px 12px;border-radius:4px;border:1px solid #333;" \
    "background:#2a2a2a;color:var(--on-surface);font-size:15px;}" \
    "input:focus{outline:none;border-color:var(--primary);}" \
    "button{margin-top:24px;width:100%;padding:12px;border:none;border-radius:4px;" \
    "background:var(--primary);color:#fff;font-size:15px;font-weight:500;" \
    "letter-spacing:.5px;cursor:pointer;}" \
    "button:active{opacity:.85;}" \
    ".row{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid #2a2a2a;}" \
    ".row:last-child{border-bottom:none;}" \
    ".row .label{color:var(--muted);font-size:13px;}" \
    ".row .value{font-weight:500;}" \
    ".badge{display:inline-block;background:var(--primary);color:#fff;border-radius:12px;" \
    "padding:2px 10px;font-size:12px;}" \
    "</style>"

static const char *SETUP_FORM_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Pop Roaster - WiFi Setup</title>"
    MATERIAL_DARK_STYLE
    "</head><body><div class='card'>"
    "<h1>Configuracao de WiFi</h1>"
    "<p>Conecte o torrador a sua rede WiFi local informando o SSID e a senha abaixo.</p>"
    "<form method='POST' action='/api/wifi/configure'>"
    "<label>SSID</label><input type='text' name='ssid' maxlength='32' required>"
    "<label>Senha</label><input type='password' name='password' maxlength='64'>"
    "<button type='submit'>Conectar</button>"
    "</form></div></body></html>";

static const char *decode_percent_inplace(char *s)
{
    char *out = s;
    for (char *in = s; *in != '\0'; ) {
        if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            char hex[3] = {in[1], in[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            in += 3;
        } else if (*in == '+') {
            *out++ = ' ';
            in++;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
    return s;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_prov_state_t state = wifi_provisioning_get_state();
    if (state == WIFI_PROV_STATE_AP_PORTAL || state == WIFI_PROV_STATE_FAILED) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SETUP_FORM_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* T043: WiFi connected - serve the full live dashboard (chart,
     * telemetry, controls, Emergency Stop, alarm banner) instead of the
     * old simple bean-temp status card. */
    dashboard_routes_send_page(req);
    return ESP_OK;
}

/* Dedicated "/wifi" route (distinct from "/") so the shared nav bar's
 * Wi-Fi Setup link always goes somewhere sensible regardless of connection
 * state - AP-portal mode shows the same setup form as "/"; once connected,
 * shows a simple status page (with the nav bar, unlike "/" which becomes
 * the dashboard once connected). */
static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    wifi_prov_state_t state = wifi_provisioning_get_state();
    if (state == WIFI_PROV_STATE_AP_PORTAL || state == WIFI_PROV_STATE_FAILED) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SETUP_FORM_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Wi-Fi</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "wifi");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'><h1>Wi-Fi</h1>"
                           "<div class='row'><span class='name'>Status</span><span class='value'>Connected</span></div>"
                           "</div></main></div></body></html>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_configure_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= FORM_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }

    char body[FORM_BODY_MAX_LEN];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    /* Password is optional (open networks). */
    httpd_query_key_value(body, "password", password, sizeof(password));

    decode_percent_inplace(ssid);
    decode_percent_inplace(password);

    esp_err_t err = wifi_provisioning_set_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_provisioning_set_credentials failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    const char *resp =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Pop Roaster - WiFi Setup</title>"
        MATERIAL_DARK_STYLE
        "</head><body><div class='card'>"
        "<h1>Credenciais salvas</h1>"
        "<p>O torrador vai tentar conectar a sua rede agora. Se a conexao falhar, "
        "ele volta automaticamente ao modo de configuracao (PopRoaster-Setup).</p>"
        "</div></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t wifi_setup_routes_register(httpd_handle_t server)
{
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &root_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t configure_uri = {
        .uri = "/api/wifi/configure",
        .method = HTTP_POST,
        .handler = wifi_configure_post_handler,
    };
    err = httpd_register_uri_handler(server, &configure_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t wifi_status_uri = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = wifi_status_get_handler,
    };
    return httpd_register_uri_handler(server, &wifi_status_uri);
}

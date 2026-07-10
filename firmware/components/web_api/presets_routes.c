/**
 * @file presets_routes.c
 * @brief See header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"

#include "web_api/presets_routes.h"
#include "web_api/dashboard_routes.h"
#include "storage/profile_store.h"

static const char *TAG = "presets_routes";

#define MAX_PROFILES_LISTED PROFILE_STORE_MAX_PROFILES
#define FORM_BODY_MAX_LEN 32

static esp_err_t presets_list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Presets</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "presets");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'><h1>Presets</h1>"
                           "<p style='color:var(--muted);font-size:13px'>Tap a preset to select it for the next roast on "
                           "the Roast tab. Creating/editing presets is only available on the device's display for now.</p>",
                           HTTPD_RESP_USE_STRLEN);

    /* Bug fix: switching presets mid-roast used to break the whole display
     * screen - profile_store now refuses the switch while a session is
     * active, and the POST handler redirects here with this query param
     * instead of silently pretending it worked. */
    char query[32];
    char error_val[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "error", error_val, sizeof(error_val));
    }
    if (strcmp(error_val, "active") == 0) {
        httpd_resp_send_chunk(req,
                               "<p style='color:var(--danger);font-weight:500'>Cannot switch preset - a roast is "
                               "currently active. Cancel or finish it first.</p>",
                               HTTPD_RESP_USE_STRLEN);
    }

    profile_store_entry_t entries[MAX_PROFILES_LISTED];
    size_t count = 0;
    profile_store_list(entries, MAX_PROFILES_LISTED, &count);

    int selected_id = -1;
    profile_store_get_selected_id(&selected_id);

    if (count == 0) {
        httpd_resp_send_chunk(req, "<p>No presets available yet.</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        for (size_t i = 0; i < count; i++) {
            bool is_selected = (entries[i].id == selected_id);
            char row[256];
            snprintf(row, sizeof(row),
                     "<form method='POST' action='/api/presets/select' style='margin:0'>"
                     "<input type='hidden' name='id' value='%d'>"
                     "<div class='row'><span class='name%s'>%s%s</span>"
                     "<button type='submit'>%s</button></div></form>",
                     entries[i].id, is_selected ? " selected" : "", entries[i].name,
                     is_selected ? " (selected)" : "", is_selected ? "Selected" : "Select");
            httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        }
    }

    httpd_resp_send_chunk(req, "</div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Presets list shown (%d profiles, selected id=%d)", (int)count, selected_id);
    return ESP_OK;
}

static esp_err_t presets_select_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= FORM_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    char body[FORM_BODY_MAX_LEN];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char id_str[8] = {0};
    if (httpd_query_key_value(body, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }
    esp_err_t err = profile_store_set_selected(atoi(id_str));

    /* Redirect back to the list so a page refresh doesn't resubmit the
     * form. profile_store now refuses the switch outright while a roast is
     * active (bug fix: switching presets mid-roast used to break the whole
     * display screen) - surface that as a query param the list page reads
     * to show an inline warning instead of silently pretending it worked. */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", err == ESP_OK ? "/presets" : "/presets?error=active");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t presets_routes_register(httpd_handle_t server)
{
    httpd_uri_t list_uri = { .uri = "/presets", .method = HTTP_GET, .handler = presets_list_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &list_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t select_uri = { .uri = "/api/presets/select", .method = HTTP_POST, .handler = presets_select_post_handler };
    err = httpd_register_uri_handler(server, &select_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Presets routes registered (/presets, /api/presets/select)");
    return ESP_OK;
}

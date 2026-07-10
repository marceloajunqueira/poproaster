/**
 * @file ota_routes.c
 * @brief See header.
 *
 * SECURITY NOTE (consistent with every other control/data endpoint in this
 * project, see server.c's FR-021 comment): no authentication is applied -
 * v1 explicitly trusts the local network the device is deployed on. A
 * firmware upload endpoint is obviously high-impact if reachable by an
 * untrusted party, but it carries no MORE risk than the existing
 * unauthenticated heater/fan control endpoint on this same trusted-network
 * assumption - not a new class of exposure for this project. Image
 * integrity itself (magic/header/checksum) is still enforced by
 * esp_ota_end() regardless of who uploads it (hal/ota_manager.c).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "web_api/ota_routes.h"
#include "web_api/dashboard_routes.h"
#include "hal/ota_manager.h"

static const char *TAG = "ota_routes";

/* Static (not stack-local): this handler's own task stack is shared with
 * every other httpd worker, and a 4KB buffer is unnecessarily large to
 * carry as a stack frame across a long-running loop - see the esp_timer
 * shared-stack lesson (repo memory) for why large scratch buffers default
 * to `static` in this codebase. Safe here too: esp_http_server serves one
 * request per worker task at a time and this buffer is only touched within
 * a single request's handler, never concurrently. */
static uint8_t s_upload_buf[4096];

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    web_ui_enable_low_latency(req);

    ota_manager_status_t status;
    ota_manager_get_status(&status);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Firmware Update</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "ota");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'>"
                           "<h1>Firmware Update</h1>"
                           "<p class='sub'>Upload a .bin built for this board (A/B OTA - the current firmware "
                           "keeps running untouched unless the new image passes validation).</p>",
                           HTTPD_RESP_USE_STRLEN);

    char row[256];
    snprintf(row, sizeof(row), "<table><tr><td>Running version</td><td>%s</td></tr>"
             "<tr><td>Running partition</td><td>%s</td></tr>"
             "<tr><td>Status</td><td>%s</td></tr></table>",
             status.running_version[0] ? status.running_version : "?",
             status.running_partition_label,
             status.pending_verify ? "Pending verification (should self-confirm shortly after boot)" : "Confirmed good");
    httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
                           "<div class='sliderrow'><input type='file' id='fwfile' accept='.bin'></div>"
                           "<div class='btnrow'><button class='primary' onclick='doUpload()'>Upload &amp; Install</button></div>"
                           "<p id='fwStatus' class='sub'></p>"
                           "</div></main></div>"
                           "<script>"
                           "function doUpload(){"
                           "var f=document.getElementById('fwfile').files[0];"
                           "if(!f){document.getElementById('fwStatus').textContent='Choose a .bin file first.';return;}"
                           "var xhr=new XMLHttpRequest();"
                           "xhr.open('POST','/api/ota/upload');"
                           "xhr.setRequestHeader('Content-Type','application/octet-stream');"
                           "xhr.upload.onprogress=function(e){"
                           "if(e.lengthComputable){"
                           "var pct=Math.round((e.loaded/e.total)*100);"
                           "document.getElementById('fwStatus').textContent='Uploading... '+pct+'%';"
                           "}};"
                           "xhr.onload=function(){"
                           "if(xhr.status===200){"
                           "document.getElementById('fwStatus').textContent='Update installed - device is rebooting. Reloading in 15s...';"
                           "setTimeout(function(){location.reload();},15000);"
                           "}else{"
                           "document.getElementById('fwStatus').textContent='Update failed: '+xhr.responseText;"
                           "}};"
                           "xhr.onerror=function(){document.getElementById('fwStatus').textContent='Upload error - connection lost.';};"
                           "xhr.send(f);"
                           "}"
                           "</script></body></html>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    ota_manager_status_t status;
    ota_manager_get_status(&status);

    char json[192];
    snprintf(json, sizeof(json), "{\"version\":\"%s\",\"partition\":\"%s\",\"pendingVerify\":%s}",
             status.running_version, status.running_partition_label,
             status.pending_verify ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    web_ui_enable_low_latency(req);

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Length");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_begin((size_t)req->content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_manager_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             err == ESP_ERR_INVALID_SIZE ? "Image too large for the target partition" : "Failed to begin OTA update");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(s_upload_buf) ? remaining : (int)sizeof(s_upload_buf);
        int received = httpd_req_recv(req, (char *)s_upload_buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; /* Transient - retry the read. */
        }
        if (received <= 0) {
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload interrupted");
            return ESP_FAIL;
        }
        err = ota_manager_write(s_upload_buf, (size_t)received);
        if (err != ESP_OK) {
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = ota_manager_end_and_activate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_manager_end_and_activate failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             "Image validation failed - update rejected, still running the previous firmware");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    ESP_LOGI(TAG, "Firmware update accepted, rebooting shortly");
    ota_manager_reboot();
    return ESP_OK;
}

esp_err_t ota_routes_register(httpd_handle_t server)
{
    httpd_uri_t get_uri = { .uri = "/ota", .method = HTTP_GET, .handler = ota_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t status_uri = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_get_handler };
    err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t upload_uri = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_post_handler };
    return httpd_register_uri_handler(server, &upload_uri);
}

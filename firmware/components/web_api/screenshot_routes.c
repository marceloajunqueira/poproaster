/**
 * @file screenshot_routes.c
 * @brief See header. Unauthenticated like every other route (FR-021 trusted-LAN assumption).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "ui_display/screenshot.h"
#include "ui_display/widgets/nav_shell.h"
#include "web_api/dashboard_routes.h"
#include "web_api/screenshot_routes.h"

static const char *TAG = "screenshot_routes";

#define BMP_HEADER_SIZE 54
#define ROWS_PER_CHUNK 8

static const char *const TAB_NAMES[NAV_SHELL_TAB_COUNT] = {
    [NAV_SHELL_TAB_ROAST] = "roast",
    [NAV_SHELL_TAB_PRESETS] = "presets",
    [NAV_SHELL_TAB_MANUAL] = "manual",
    [NAV_SHELL_TAB_HISTORY] = "history",
    [NAV_SHELL_TAB_CONFIG] = "config",
};

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    int tab = -1;
    char query[48];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "tab", value, sizeof(value)) == ESP_OK) {
        for (int i = 0; i < NAV_SHELL_TAB_COUNT; i++) {
            if (strcmp(value, TAB_NAMES[i]) == 0) {
                tab = i;
                break;
            }
        }
        if (tab < 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown tab");
        }
    }

    ui_display_screenshot_t shot;
    esp_err_t err = ui_display_screenshot_capture(tab, &shot);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Cannot switch tabs while a roast is active");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Screenshot failed");
    }

    const uint32_t row_bytes = (uint32_t)shot.width * 3; /* 480*3 is already 4-byte aligned */
    const uint32_t pixel_bytes = row_bytes * shot.height;

    uint8_t header[BMP_HEADER_SIZE] = {0};
    header[0] = 'B';
    header[1] = 'M';
    put_le32(&header[2], BMP_HEADER_SIZE + pixel_bytes);
    put_le32(&header[10], BMP_HEADER_SIZE);
    put_le32(&header[14], 40);
    put_le32(&header[18], shot.width);
    put_le32(&header[22], shot.height); /* positive = rows stored bottom-up */
    header[26] = 1;
    header[28] = 24;
    put_le32(&header[34], pixel_bytes);

    uint8_t *chunk = malloc(row_bytes * ROWS_PER_CHUNK);
    if (chunk == NULL) {
        ui_display_screenshot_free(&shot);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    char disposition[64];
    snprintf(disposition, sizeof(disposition), "inline; filename=\"screen_%s.bmp\"",
             tab >= 0 ? TAB_NAMES[tab] : "current");
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t send_err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    int y = shot.height - 1;
    while (send_err == ESP_OK && y >= 0) {
        int rows = 0;
        for (; rows < ROWS_PER_CHUNK && y >= 0; rows++, y--) {
            ui_display_screenshot_row_bgr(&shot, y, chunk + (size_t)rows * row_bytes);
        }
        send_err = httpd_resp_send_chunk(req, (const char *)chunk, rows * row_bytes);
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, NULL, 0);
    }

    free(chunk);
    ui_display_screenshot_free(&shot);
    return send_err;
}

static esp_err_t screen_page_get_handler(httpd_req_t *req)
{
    web_ui_enable_low_latency(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Screen Capture</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "screen");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'>"
                           "<h1>Screen Capture</h1>"
                           "<p class='sub'>Captures what the device LCD is showing right now. Choosing a tab switches the LCD "
                           "to it for a moment and then back (not possible while a roast is active).</p>"
                           "<div class='btnrow'>"
                           "<button class='primary' onclick=\"cap('')\">Current screen</button>"
                           "<button onclick=\"cap('roast')\">Roast</button>"
                           "<button onclick=\"cap('presets')\">Presets</button>"
                           "<button onclick=\"cap('manual')\">Manual</button>"
                           "<button onclick=\"cap('history')\">History</button>"
                           "<button onclick=\"cap('config')\">Config</button>"
                           "<button id='dl' onclick='dlPng()' disabled>Download PNG</button>"
                           "</div>"
                           "<p id='st' class='sub'></p>"
                           "<img id='shot' alt='' style='max-width:100%;border:1px solid #444;display:none'>"
                           "</div></main></div>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<script>"
                           "var cur=null,curName='screen';"
                           "function cap(tab){"
                           "var st=document.getElementById('st'),img=document.getElementById('shot');"
                           "st.textContent='Capturing...';"
                           "var url='/api/screenshot?t='+Date.now()+(tab?'&tab='+tab:'');"
                           "fetch(url).then(function(r){"
                           "if(!r.ok){return r.text().then(function(t){throw new Error(t||r.status);});}"
                           "return r.blob();})"
                           ".then(function(b){"
                           "if(cur){URL.revokeObjectURL(cur);}"
                           "cur=URL.createObjectURL(b);curName='screen_'+(tab||'current');"
                           "img.src=cur;img.style.display='block';"
                           "document.getElementById('dl').disabled=false;st.textContent='';})"
                           ".catch(function(e){st.textContent='Failed: '+e.message;});"
                           "}"
                           "function dlPng(){"
                           "var img=document.getElementById('shot');"
                           "var c=document.createElement('canvas');"
                           "c.width=img.naturalWidth;c.height=img.naturalHeight;"
                           "c.getContext('2d').drawImage(img,0,0);"
                           "c.toBlob(function(b){"
                           "var a=document.createElement('a');a.href=URL.createObjectURL(b);"
                           "a.download=curName+'.png';a.click();"
                           "setTimeout(function(){URL.revokeObjectURL(a.href);},1000);"
                           "},'image/png');"
                           "}"
                           "</script></body></html>",
                           HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t screenshot_routes_register(httpd_handle_t server)
{
    static const httpd_uri_t uri = {
        .uri = "/api/screenshot",
        .method = HTTP_GET,
        .handler = screenshot_get_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/screen",
        .method = HTTP_GET,
        .handler = screen_page_get_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &page_uri);
}

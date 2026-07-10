/**
 * @file history_routes.c
 * @brief See header.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"

#include "web_api/history_routes.h"
#include "web_api/dashboard_routes.h"
#include "storage/session_store.h"

static const char *TAG = "history_routes";

#define MAX_SESSIONS_LISTED 30
#define CHART_MAX_POINTS 100
#define FALLBACK_DURATION_S 60

/* Same "Roast #<N> - <Preset>" convention as ui_display/screens/session_review.c's
 * format_session_display_name() - duplicated here rather than sharing code
 * across components (web_api has no business depending on the LVGL-coupled
 * ui_display component just for this one small formatter). */
static void format_display_name(const char *session_id, char *out, size_t out_len)
{
    session_meta_t meta;
    if (session_store_load_meta(session_id, &meta) == ESP_OK) {
        if (meta.has_profile) {
            snprintf(out, out_len, "Roast #%lu - %s", (unsigned long)meta.roast_number, meta.profile.name);
        } else {
            snprintf(out, out_len, "Roast #%lu", (unsigned long)meta.roast_number);
        }
    } else {
        strncpy(out, session_id, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static esp_err_t history_list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Roast History</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "history");
    httpd_resp_send_chunk(req, "<main class='content'><div class='card'><h1>Roast History</h1>", HTTPD_RESP_USE_STRLEN);

    static char ids[MAX_SESSIONS_LISTED][SESSION_STORE_ID_MAX_LEN];
    size_t count = 0;
    session_store_list_sessions(ids, MAX_SESSIONS_LISTED, &count);

    if (count == 0) {
        httpd_resp_send_chunk(req, "<p>No completed roasts yet.</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        for (size_t i = 0; i < count; i++) {
            char display_name[80];
            format_display_name(ids[i], display_name, sizeof(display_name));
            char row[400];
            snprintf(row, sizeof(row),
                     "<div class='row'><a class='name' href='/history/detail?id=%s'>%s</a>"
                     "<a class='btnlink' href='/api/sessions/export?id=%s&format=csv'>CSV</a></div>",
                     ids[i], display_name, ids[i]);
            httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        }
    }

    httpd_resp_send_chunk(req, "</div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Roast history list shown (%d sessions)", (int)count);
    return ESP_OK;
}

static void append_float_array(char *buf, size_t buf_size, size_t *offset, const char *var_name, const float *data, int n)
{
    if (*offset >= buf_size) {
        return;
    }
    *offset += snprintf(buf + *offset, buf_size - *offset, "var %s=[", var_name);
    for (int i = 0; i < n && *offset < buf_size; i++) {
        *offset += snprintf(buf + *offset, buf_size - *offset, "%s%.1f", i > 0 ? "," : "", data[i]);
    }
    if (*offset < buf_size) {
        *offset += snprintf(buf + *offset, buf_size - *offset, "];");
    }
}

static esp_err_t history_detail_get_handler(httpd_req_t *req)
{
    char query[160];
    char session_id[SESSION_STORE_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", session_id, sizeof(session_id));
    }
    if (session_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    char display_name[80];
    format_display_name(session_id, display_name, sizeof(display_name));

    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Roast History</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "history");
    httpd_resp_send_chunk(req, "<main class='content'><div class='card'>", HTTPD_RESP_USE_STRLEN);

    char title_html[256];
    snprintf(title_html, sizeof(title_html), "<h1>%s</h1><p><a class='btnlink' href='/history'>&larr; Back</a></p>", display_name);
    httpd_resp_send_chunk(req, title_html, HTTPD_RESP_USE_STRLEN);

    FILE *f = session_store_open_session(session_id);
    if (f == NULL) {
        httpd_resp_send_chunk(req, "<p>Could not open session file.</p></div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char line[160];
    int64_t max_t_ms = 0;
    float max_bt = 0.0f, max_ror = 0.0f;
    long heater_sum = 0, fan_sum = 0;
    size_t sample_count = 0;
    bool any_sample = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            if (t > max_t_ms) max_t_ms = t;
            if (bt > max_bt) max_bt = bt;
            if (ror > max_ror) max_ror = ror;
            heater_sum += heater;
            fan_sum += fan;
            sample_count++;
            any_sample = true;
        }
    }

    if (!any_sample) {
        fclose(f);
        httpd_resp_send_chunk(req, "<p>No telemetry recorded for this session.</p></div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    uint32_t duration_s = (uint32_t)(max_t_ms / 1000);
    if (duration_s == 0) {
        duration_s = FALLBACK_DURATION_S;
    }

    static float bt_pts[CHART_MAX_POINTS];
    static float fan_pts[CHART_MAX_POINTS];
    memset(bt_pts, 0, sizeof(bt_pts));
    memset(fan_pts, 0, sizeof(fan_pts));

    rewind(f);
    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            int idx = (int)((t / 1000) * (CHART_MAX_POINTS - 1) / duration_s);
            if (idx < 0) idx = 0;
            if (idx >= CHART_MAX_POINTS) idx = CHART_MAX_POINTS - 1;
            bt_pts[idx] = bt;
            fan_pts[idx] = fan;
        }
    }
    fclose(f);

    int64_t total_s = max_t_ms / 1000;
    long avg_heater = (sample_count > 0) ? (heater_sum / (long)sample_count) : 0;
    long avg_fan = (sample_count > 0) ? (fan_sum / (long)sample_count) : 0;

    char stats_html[1536];
    snprintf(stats_html, sizeof(stats_html),
             "<canvas id='chart'></canvas>"
             "<div class='legend'><span class='bt'>&#9679; BT</span> &nbsp; <span class='fan'>&#9679; Fan</span>"
             " &nbsp; (no target-curve overlay on web yet, see the display's Roast History for that)</div>"
             "<div class='grid'>"
             "<div class='stat'><div class='label'>Total time</div><div class='value'>%02d:%02d</div></div>"
             "<div class='stat'><div class='label'>Max BT</div><div class='value'>%.1f C</div></div>"
             "<div class='stat'><div class='label'>Max RoR</div><div class='value'>%.1f C/min</div></div>"
             "<div class='stat'><div class='label'>Avg Fan / Heater</div><div class='value'>%ld%% / %ld%%</div></div>"
             "</div>",
             (int)(total_s / 60), (int)(total_s % 60), max_bt, max_ror, avg_fan, avg_heater);
    httpd_resp_send_chunk(req, stats_html, HTTPD_RESP_USE_STRLEN);

    static char script_buf[4096];
    size_t offset = 0;
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "<script>");
    append_float_array(script_buf, sizeof(script_buf), &offset, "btData", bt_pts, CHART_MAX_POINTS);
    append_float_array(script_buf, sizeof(script_buf), &offset, "fanData", fan_pts, CHART_MAX_POINTS);
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset,
                        "var chart=document.getElementById('chart');var ctx=chart.getContext('2d');"
                        "function draw(){chart.width=chart.clientWidth;chart.height=180;"
                        "var w=chart.width,h=chart.height;ctx.clearRect(0,0,w,h);"
                        "ctx.strokeStyle='#333';ctx.lineWidth=1;ctx.beginPath();"
                        "for(var i=1;i<4;i++){var y=h*i/4;ctx.moveTo(0,y);ctx.lineTo(w,y);}ctx.stroke();"
                        "function plot(data,max,color){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
                        "for(var i=0;i<data.length;i++){var x=w*i/(data.length-1);var y=h-(data[i]/max)*h;"
                        "if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}ctx.stroke();}"
                        "plot(btData,260,'#FF9746');plot(fanData,100,'#66BB6A');}"
                        "draw();window.addEventListener('resize',draw);"
                        "</script>");
    httpd_resp_send_chunk(req, script_buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "</div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* T048: session export endpoint - CSV only for now (a .alog/Artisan-format
 * exporter is a reasonable future addition once the Artisan bridge, T047,
 * actually exists and there's a concrete consumer to validate the format
 * against). */
static esp_err_t sessions_export_get_handler(httpd_req_t *req)
{
    char query[160];
    char session_id[SESSION_STORE_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", session_id, sizeof(session_id));
    }
    if (session_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    FILE *f = session_store_open_session(session_id);
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/csv");
    char disposition[80];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s.csv\"", session_id);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    httpd_resp_send_chunk(req, "t_ms,bt_c,ror_c_per_min,fan_pct,heater_pct,phase\n", HTTPD_RESP_USE_STRLEN);

    char line[160];
    char csv_line[96];
    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            int n = snprintf(csv_line, sizeof(csv_line), "%lld,%.1f,%.1f,%d,%d,%d\n", t, bt, ror, fan, heater, phase);
            httpd_resp_send_chunk(req, csv_line, n);
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t history_routes_register(httpd_handle_t server)
{
    httpd_uri_t list_uri = { .uri = "/history", .method = HTTP_GET, .handler = history_list_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &list_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t detail_uri = { .uri = "/history/detail", .method = HTTP_GET, .handler = history_detail_get_handler };
    err = httpd_register_uri_handler(server, &detail_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t export_uri = { .uri = "/api/sessions/export", .method = HTTP_GET, .handler = sessions_export_get_handler };
    err = httpd_register_uri_handler(server, &export_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "History routes registered (/history, /history/detail, /api/sessions/export)");
    return ESP_OK;
}

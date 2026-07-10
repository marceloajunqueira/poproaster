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
#define HISTORY_MAX_EVENTS 16

typedef struct {
    int64_t total_s;
    float max_bt;
    float max_ror;
    long avg_fan;
    long avg_heater;
    bool any_sample;
} session_stats_t;

/* Single-pass summary stats (Total time/Max BT/Max RoR/Avg Fan/Avg Heater)
 * for one session's .jsonl file - shared by both the list page's per-card
 * summary and the detail page's own stats panel. Note: called once per
 * listed session on every /history page load (up to MAX_SESSIONS_LISTED),
 * each a full sequential read of that session's telemetry file - fine for
 * typical roast lengths, but a page with many long (20-25min) roasts could
 * feel slower to load than a plain list would; no caching implemented here,
 * worth revisiting if that's ever reported as an issue. */
static void compute_session_stats(const char *session_id, session_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = session_store_open_session(session_id);
    if (f == NULL) {
        return;
    }
    char line[160];
    int64_t max_t_ms = 0;
    long heater_sum = 0, fan_sum = 0;
    size_t sample_count = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            if (t > max_t_ms) max_t_ms = t;
            if (bt > out->max_bt) out->max_bt = bt;
            if (ror > out->max_ror) out->max_ror = ror;
            heater_sum += heater;
            fan_sum += fan;
            sample_count++;
            out->any_sample = true;
        }
    }
    fclose(f);
    out->total_s = max_t_ms / 1000;
    out->avg_fan = (sample_count > 0) ? (fan_sum / (long)sample_count) : 0;
    out->avg_heater = (sample_count > 0) ? (heater_sum / (long)sample_count) : 0;
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
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "history");
    httpd_resp_send_chunk(req, "<main class='content'><div class='card'><h1>Roast History</h1>", HTTPD_RESP_USE_STRLEN);

    static char ids[MAX_SESSIONS_LISTED][SESSION_STORE_ID_MAX_LEN];
    size_t count = 0;
    session_store_list_sessions(ids, MAX_SESSIONS_LISTED, &count);

    if (count == 0) {
        httpd_resp_send_chunk(req, "<p>No completed roasts yet.</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        /* "Catalog" of small cards instead of the old plain list (operator-
         * reported: a bare blue-on-black hyperlink per row looked broken).
         * Shows the roast name/preset plus whatever Batch Info (coffee/
         * origin/weight) was filled in, if any - the main data at a
         * glance, matching the requested catalog look. */
        httpd_resp_send_chunk(req, "<div class='history-grid'>", HTTPD_RESP_USE_STRLEN);
        static char card[1536]; /* static: see server.c's stack_size crash-fix comment; generously sized vs. gcc's format-truncation worst-case for the embedded display_name/meta_html/stats_mini_html/id strings */
        for (size_t i = 0; i < count; i++) {
            char display_name[80];
            char meta_html[384] = ""; /* generous vs. gcc's format-truncation worst-case estimate for %.0f on a float */
            session_meta_t meta;
            if (session_store_load_meta(ids[i], &meta) == ESP_OK) {
                if (meta.has_profile) {
                    snprintf(display_name, sizeof(display_name), "Roast #%lu - %s", (unsigned long)meta.roast_number,
                             meta.profile.name);
                } else {
                    snprintf(display_name, sizeof(display_name), "Roast #%lu", (unsigned long)meta.roast_number);
                }
                bool has_batch = (meta.batch.coffee_name[0] != '\0' || meta.batch.origin[0] != '\0' ||
                                   meta.batch.weight_g > 0.0f);
                if (has_batch) {
                    snprintf(meta_html, sizeof(meta_html), "<div class='meta'>%s%s%s%s%.0f g</div>",
                             meta.batch.coffee_name[0] ? meta.batch.coffee_name : "",
                             meta.batch.origin[0] ? " - " : "", meta.batch.origin[0] ? meta.batch.origin : "",
                             meta.batch.weight_g > 0.0f ? " - " : "", (double)meta.batch.weight_g);
                }
            } else {
                strncpy(display_name, ids[i], sizeof(display_name) - 1);
                display_name[sizeof(display_name) - 1] = '\0';
            }

            session_stats_t stats;
            compute_session_stats(ids[i], &stats);
            char stats_mini_html[512] = "";
            if (stats.any_sample) {
                /* Icon + value "tiles" in a 2-column grid instead of plain
                 * inline text (operator-reported: everything ran together
                 * with no separation/organization). Plain Unicode emoji,
                 * no external icon font - same convention as the nav bar. */
                snprintf(stats_mini_html, sizeof(stats_mini_html),
                         "<div class='stats-mini'>"
                         "<div class='stat-mini'><span class='icon'>&#9201;</span><b>%02d:%02d</b></div>"
                         "<div class='stat-mini'><span class='icon'>&#127777;</span><b>%.1f&#8451;</b></div>"
                         "<div class='stat-mini'><span class='icon'>&#128200;</span><b>%.1f/min</b></div>"
                         "<div class='stat-mini'><span class='icon'>&#128168;</span><b>%ld%%</b>"
                         "&nbsp;<span class='icon'>&#128293;</span><b>%ld%%</b></div>"
                         "</div>",
                         (int)(stats.total_s / 60), (int)(stats.total_s % 60), (double)stats.max_bt,
                         (double)stats.max_ror, stats.avg_fan, stats.avg_heater);
            }

            snprintf(card, sizeof(card),
                     "<div class='history-card'><div class='title'>%s</div>%s%s"
                     "<div class='actions'><a class='btnlink primary' href='/history/detail?id=%s'>View</a>"
                     "<a class='btnlink' href='/api/sessions/export?id=%s&format=csv'>CSV</a></div></div>",
                     display_name, meta_html, stats_mini_html, ids[i], ids[i]);
            httpd_resp_send_chunk(req, card, HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
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
    session_meta_t meta;
    bool has_meta = (session_store_load_meta(session_id, &meta) == ESP_OK);
    if (has_meta) {
        if (meta.has_profile) {
            snprintf(display_name, sizeof(display_name), "Roast #%lu - %s", (unsigned long)meta.roast_number,
                     meta.profile.name);
        } else {
            snprintf(display_name, sizeof(display_name), "Roast #%lu", (unsigned long)meta.roast_number);
        }
    } else {
        strncpy(display_name, session_id, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
    }
    bool has_profile = has_meta && meta.has_profile;

    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Roast History</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "history");
    httpd_resp_send_chunk(req, "<main class='content'><div class='card'>", HTTPD_RESP_USE_STRLEN);

    static char title_html[256]; /* static: see server.c's stack_size crash-fix comment */
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

    /* Marked-event lines ("ev") are interleaved with telemetry lines in the
     * same .jsonl file - collect them here (bounded, matches
     * ROAST_EVENTS_MAX=16) so they can be drawn as vertical reference
     * markers on the chart, same as the on-device Roast History and the
     * live web dashboard. */
    int64_t event_t_ms[HISTORY_MAX_EVENTS];
    int event_type[HISTORY_MAX_EVENTS];
    size_t event_count = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "\"ev\":") != NULL) {
            long long t = 0;
            int type = 0;
            if (event_count < HISTORY_MAX_EVENTS && sscanf(line, "{\"t\":%lld,\"ev\":%d}", &t, &type) == 2) {
                event_t_ms[event_count] = t;
                event_type[event_count] = type;
                event_count++;
            }
            continue;
        }
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

    /* Same convention as the on-device Roast History (session_review.c):
     * when a profile snapshot exists, the CHART TIMELINE spans the
     * profile's own total programmed duration (so the dashed target curve
     * always shows its complete, correct shape) rather than however long
     * the roast actually ran for (which can be shorter, e.g. cancelled
     * early, or longer). Falls back to the recorded duration, then a
     * fixed fallback, if there's no profile snapshot. */
    uint32_t duration_s = has_profile ? roast_profile_total_duration_s(&meta.profile) : 0;
    if (duration_s == 0) {
        duration_s = (uint32_t)(max_t_ms / 1000);
    }
    if (duration_s == 0) {
        duration_s = FALLBACK_DURATION_S;
    }

    /* -1.0f sentinel ("no sample at this index") rather than 0.0f - a real
     * BT reading is never negative, but IS often legitimately near 0 right
     * after CHARGE, so 0.0f can't safely double as "missing data" (the
     * previous code used 0.0f and the chart line would visibly plunge to
     * zero at every unfilled index instead of just not being drawn there). */
    static float bt_pts[CHART_MAX_POINTS];
    for (int i = 0; i < CHART_MAX_POINTS; i++) {
        bt_pts[i] = -1.0f;
    }

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
        }
    }
    fclose(f);

    int64_t total_s = max_t_ms / 1000;
    long avg_heater = (sample_count > 0) ? (heater_sum / (long)sample_count) : 0;
    long avg_fan = (sample_count > 0) ? (fan_sum / (long)sample_count) : 0;

    static char stats_html[1536]; /* static: see server.c's stack_size crash-fix comment */
    snprintf(stats_html, sizeof(stats_html),
             "<canvas id='chart'></canvas>"
             "<div class='legend'><span class='bt'>&#9679; BT</span>"
             " &nbsp; <span style='color:var(--muted)'>(dashed = profile target, incl. Fan - no solid Fan line,"
             " this hardware has no RPM sensor, Avg Fan%% below is the commanded value, not measured)</span></div>"
             "<div class='grid'>"
             "<div class='stat'><div class='label'>Total time</div><div class='value'>%02d:%02d</div></div>"
             "<div class='stat'><div class='label'>Max BT</div><div class='value'>%.1f C</div></div>"
             "<div class='stat'><div class='label'>Max RoR</div><div class='value'>%.1f C/min</div></div>"
             "<div class='stat'><div class='label'>Avg Fan / Heater</div><div class='value'>%ld%% / %ld%%</div></div>"
             "</div>",
             (int)(total_s / 60), (int)(total_s % 60), max_bt, max_ror, avg_fan, avg_heater);
    httpd_resp_send_chunk(req, stats_html, HTTPD_RESP_USE_STRLEN);

    static char script_buf[6144]; /* generous margin - now embeds profileSegments + the target-curve JS on top of btData/eventMarkers */
    size_t offset = 0;
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "<script>");
    append_float_array(script_buf, sizeof(script_buf), &offset, "btData", bt_pts, CHART_MAX_POINTS);
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "var durationS=%lu;", (unsigned long)duration_s);

    /* Dashed target-curve overlay (temp+fan), same technique as the live
     * dashboard - previously this page had none at all. Segments embedded
     * as a plain JS array; computeTargetCurve() (below) fills
     * ttempData/tfanData at CHART_MAX_POINTS resolution to match btData. */
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "var profileSegments=[");
    if (has_profile) {
        for (uint8_t i = 0; i < meta.profile.point_count && offset < sizeof(script_buf) - 48; i++) {
            offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "%s{\"d\":%lu,\"t\":%.1f,\"f\":%u}",
                               i == 0 ? "" : ",", (unsigned long)meta.profile.points[i].duration_s,
                               (double)meta.profile.points[i].target_temp_c,
                               (unsigned)meta.profile.points[i].target_fan_pct);
        }
    }
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "];");

    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "var eventMarkers=[");
    for (size_t i = 0; i < event_count; i++) {
        offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "%s[%lld,%d]", i == 0 ? "" : ",",
                            (long long)event_t_ms[i], event_type[i]);
    }
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset, "];");
    offset += snprintf(script_buf + offset, sizeof(script_buf) - offset,
                        "var EVENT_LABELS=['TP','FCs','FCe','SCs','SCe','Cool'];"
                        "var MAXPTS=%d;"
                        "var ttempData=new Array(MAXPTS).fill(null);"
                        "var tfanData=new Array(MAXPTS).fill(null);"
                        "function computeTargetCurve(){"
                        "if(!profileSegments||profileSegments.length===0)return;"
                        "for(var i=0;i<MAXPTS;i++){"
                        "var t=i*durationS/(MAXPTS-1);var cursor=0,temp=0,fan=0;"
                        "for(var s=0;s<profileSegments.length;s++){var seg=profileSegments[s];"
                        "if(t<cursor+seg.d||s===profileSegments.length-1){temp=seg.t;fan=seg.f;break;}"
                        "cursor+=seg.d;}"
                        "ttempData[i]=temp;tfanData[i]=fan;}"
                        "}"
                        "computeTargetCurve();"
                        "var chart=document.getElementById('chart');var ctx=chart.getContext('2d');"
                        "var PAD_TOP=14,PAD_BOTTOM=14;"
                        "function draw(){chart.width=chart.clientWidth;chart.height=260;"
                        "var w=chart.width,h=chart.height;ctx.clearRect(0,0,w,h);"
                        "ctx.strokeStyle='#333';ctx.lineWidth=1;ctx.beginPath();"
                        "for(var i=1;i<4;i++){var y=PAD_TOP+(h-PAD_TOP-PAD_BOTTOM)*i/4;ctx.moveTo(0,y);ctx.lineTo(w,y);}ctx.stroke();"
                        "function plot(data,max,color,dashed){ctx.setLineDash(dashed?[5,5]:[]);ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
                        "var started=false;"
                        "for(var i=0;i<data.length;i++){"
                        /* null/-1 sentinel ("no sample here") - skip WITHOUT
                         * breaking the line, same continuous-line fix as
                         * the live dashboard (operator-reported: the line
                         * looked like disconnected dots). */
                        "if(data[i]===null||data[i]===undefined||data[i]<0){continue;}"
                        "var x=w*i/(data.length-1);var y=PAD_TOP+(1-data[i]/max)*(h-PAD_TOP-PAD_BOTTOM);"
                        "if(!started){ctx.moveTo(x,y);started=true;}else{ctx.lineTo(x,y);}}ctx.stroke();ctx.setLineDash([]);}"
                        "plot(ttempData,260,'#FF9746',true);plot(tfanData,100,'#66BB6A',true);"
                        "plot(btData,260,'#FF9746',false);"
                        "ctx.font='10px sans-serif';"
                        "for(var i=0;i<eventMarkers.length;i++){"
                        "var ev=eventMarkers[i];var x=w*(ev[0]/1000)/durationS;if(x<0)x=0;if(x>w)x=w;"
                        "ctx.setLineDash([3,3]);ctx.strokeStyle='#BA68C8';ctx.lineWidth=1;"
                        "ctx.beginPath();ctx.moveTo(x,PAD_TOP);ctx.lineTo(x,h-PAD_BOTTOM);ctx.stroke();ctx.setLineDash([]);"
                        "ctx.fillStyle='#BA68C8';ctx.fillText(EVENT_LABELS[ev[1]]||'?',x+2,PAD_TOP+10);"
                        "}"
                        "}"
                        "draw();window.addEventListener('resize',draw);"
                        "</script>",
                        CHART_MAX_POINTS);
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

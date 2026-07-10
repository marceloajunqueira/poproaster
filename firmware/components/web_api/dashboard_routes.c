/**
 * @file dashboard_routes.c
 * @brief See header.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "web_api/dashboard_routes.h"
#include "roast_core/session_state_machine.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/roast_events.h"
#include "storage/profile_store.h"
#include "safety/safety_manager.h"

static const char *TAG = "dashboard_routes";

#define MAX_WS_CLIENTS 4
#define CONTROL_BODY_MAX_LEN 128
#define WS_BROADCAST_PERIOD_US (500 * 1000) /* 500ms (2Hz) - T059: matches the telemetry
                                              * service's own sample rate and the on-device
                                              * dashboard's lv_timer refresh cadence, so web
                                              * clients see updates just as often as the
                                              * physical display (within the 2-5Hz target). */

static httpd_handle_t s_server_handle;
static int s_ws_fds[MAX_WS_CLIENTS];
static esp_timer_handle_t s_broadcast_timer;

void web_ui_enable_low_latency(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return;
    }
    int nodelay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        ESP_LOGD(TAG, "setsockopt(TCP_NODELAY) failed for fd=%d", fd);
    }
}

/* Serves the shared stylesheet on its own cacheable route instead of every
 * page re-sending it inline (see WEB_UI_STYLE_LINK doc comment) - this was
 * a major contributor to the web UI feeling slow to navigate: several KB of
 * identical CSS text no longer needs to be re-transferred on every single
 * page load once the browser has cached it once. */
static esp_err_t style_css_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    httpd_resp_send(req, WEB_UI_STYLE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void web_ui_send_nav_bar(httpd_req_t *req, const char *active_page)
{
    httpd_resp_send_chunk(req, "<nav class='sidebar'><div class='brand'>Pop Roaster</div>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "dashboard") == 0
                               ? "<a class='active' href='/'><span class='navicon'>&#9749;</span><span>Dashboard</span></a>"
                               : "<a href='/'><span class='navicon'>&#9749;</span><span>Dashboard</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "history") == 0
                               ? "<a class='active' href='/history'><span class='navicon'>&#128220;</span><span>Roast History</span></a>"
                               : "<a href='/history'><span class='navicon'>&#128220;</span><span>Roast History</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "presets") == 0
                               ? "<a class='active' href='/presets'><span class='navicon'>&#9881;</span><span>Presets</span></a>"
                               : "<a href='/presets'><span class='navicon'>&#9881;</span><span>Presets</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "wifi") == 0
                               ? "<a class='active' href='/wifi'><span class='navicon'>&#128225;</span><span>Wi-Fi Setup</span></a>"
                               : "<a href='/wifi'><span class='navicon'>&#128225;</span><span>Wi-Fi Setup</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "diagnostics") == 0
                               ? "<a class='active' href='/diagnostics'><span class='navicon'>&#128202;</span><span>Diagnostics</span></a>"
                               : "<a href='/diagnostics'><span class='navicon'>&#128202;</span><span>Diagnostics</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           strcmp(active_page, "ota") == 0
                               ? "<a class='active' href='/ota'><span class='navicon'>&#8593;</span><span>Firmware Update</span></a>"
                               : "<a href='/ota'><span class='navicon'>&#8593;</span><span>Firmware Update</span></a>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</nav>", HTTPD_RESP_USE_STRLEN);
}

/* Operator-reported bug: opening/refreshing the dashboard mid-roast only
 * ever showed telemetry broadcast AFTER the page connected - all points
 * recorded before that were lost, since the chart's btData array always
 * starts out empty (all-null) and only fills in as new WebSocket messages
 * arrive. Fix: this endpoint returns whatever's ALREADY been recorded for
 * the currently-active session, so the page's own JS can backfill the
 * chart once at load time. Returns a JSON array of [t_ms, bt] pairs (empty
 * array if no roast is currently being recorded).
 *
 * IMPORTANT (2nd revision): this used to re-read/re-parse the session's
 * WHOLE .jsonl telemetry file (two passes: one to count lines, one to
 * emit a downsampled subset) on every single page load/refresh. That was
 * still too slow on a long-enough roast - the single httpd worker task
 * could stall badly enough that a mid-roast refresh stopped responding
 * entirely ("nao abriu mais"). Replaced with
 * roast_telemetry_service_get_live_chart_points(): a small, BOUNDED,
 * IN-RAM buffer (one point appended roughly every 10s during an active
 * roast - see LIVE_CHART_SAMPLE_PERIOD_MS/LIVE_CHART_MAX_POINTS in
 * roast_telemetry_service.h) that this handler just copies out. No file
 * I/O at all in this request path anymore - fast and safe (can't hang)
 * regardless of roast length; the full-resolution .jsonl recording still
 * happens as always, just isn't what THIS endpoint reads from. */
static esp_err_t live_history_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    web_ui_enable_low_latency(req);

    static live_chart_point_t points[LIVE_CHART_MAX_POINTS];
    size_t n = roast_telemetry_service_get_live_chart_points(points, LIVE_CHART_MAX_POINTS);

    /* Also backfill event markers (Turning Point/FC/SC/Cool Start) - same
     * in-RAM roast_events_get_all() source the WS broadcast uses, so a
     * page opened/refreshed mid-roast sees markers already placed, not
     * just ones marked AFTER it (re)connects. */
    roast_event_record_t events[ROAST_EVENTS_MAX];
    size_t event_count = roast_events_get_all(events, ROAST_EVENTS_MAX);

    static char buf[LIVE_CHART_MAX_POINTS * 24 + ROAST_EVENTS_MAX * 20 + 32];
    size_t len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "{\"pts\":[");
    for (size_t i = 0; i < n; i++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s[%lld,%.1f]", i == 0 ? "" : ",",
                         (long long)points[i].elapsed_ms, (double)points[i].bean_temp_c);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "],\"events\":[");
    for (size_t i = 0; i < event_count; i++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s[%lld,%d]", i == 0 ? "" : ",",
                         (long long)events[i].elapsed_ms, (int)events[i].type);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static const char *phase_str(roast_phase_t phase)
{
    switch (phase) {
    case ROAST_PHASE_IDLE: return "IDLE";
    case ROAST_PHASE_PREHEAT: return "PREHEAT";
    case ROAST_PHASE_ROASTING: return "ROASTING";
    case ROAST_PHASE_DEVELOPMENT: return "DEVELOPMENT";
    case ROAST_PHASE_COOLING: return "COOLING";
    case ROAST_PHASE_COMPLETED: return "COMPLETED";
    case ROAST_PHASE_ABORTED: return "ABORTED";
    default: return "?";
    }
}

static const char *alarm_str(safety_alarm_type_t alarm)
{
    switch (alarm) {
    case SAFETY_ALARM_TEMP_ABSOLUTE_CUTOFF: return "TEMP CUTOFF: bean temp reached 260C, heater OFF";
    case SAFETY_ALARM_SENSOR_FAILURE: return "SENSOR FAILURE: invalid temperature reading, heater OFF";
    case SAFETY_ALARM_FAN_FAILURE_INDIRECT: return "FAN FAILURE: abnormal heating pattern detected, heater OFF";
    case SAFETY_ALARM_DURATION_WATCHDOG: return "MAX DURATION REACHED: auto-cooling forced";
    case SAFETY_ALARM_EMERGENCY_STOP: return "EMERGENCY STOP activated";
    default: return "";
    }
}

/* ---- T041: WebSocket telemetry broadcast ---- */

static void register_ws_client(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            return; /* Already tracked. */
        }
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == 0) {
            s_ws_fds[i] = fd;
            ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
            return;
        }
    }
    ESP_LOGW(TAG, "WS client (fd=%d) dropped - already at MAX_WS_CLIENTS (%d)", fd, MAX_WS_CLIENTS);
}

static void unregister_ws_client(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = 0;
            ESP_LOGI(TAG, "WS client disconnected (fd=%d)", fd);
            return;
        }
    }
}

static esp_err_t ws_telemetry_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Initial handshake - httpd completes the WS upgrade automatically;
         * just start tracking this client's socket for broadcasting. */
        register_ws_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* A frame arrived from the client - we don't need anything FROM the
     * browser (control commands go through the separate POST endpoint), so
     * just drain it; detect CLOSE frames to stop tracking this client. */
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0); /* First call: just get the length. */
    if (err != ESP_OK) {
        unregister_ws_client(httpd_req_to_sockfd(req));
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        unregister_ws_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.len > 0 && frame.len < 16) {
        uint8_t buf[16];
        frame.payload = buf;
        httpd_ws_recv_frame(req, &frame, sizeof(buf)); /* Drain and discard. */
    }
    return ESP_OK;
}

static void ws_broadcast_timer_cb(void *arg)
{
    (void)arg;
    bool any_client = false;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != 0) {
            any_client = true;
            break;
        }
    }
    if (!any_client) {
        return; /* Nobody listening - skip the (cheap but non-zero) snapshot work. */
    }

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);
    const roast_session_t *session = session_sm_get_state();
    bool needs_ack = false;
    safety_alarm_type_t alarm = safety_manager_get_active_alarm(&needs_ack);

    /* Target Bean Temp/Fan (TBT) + the full segment curve (for the dashed
     * target-curve overlay), same as the on-device display shows. Sent on
     * EVERY tick (not just once at page load) so the web dashboard stays
     * live-synced if the operator changes the selected preset elsewhere
     * (display, or another browser tab) while this page stays open -
     * previously the curve was only ever embedded once at initial page
     * load, so it silently went stale until a manual refresh.
     *
     * NOTE: this callback runs on esp_timer's single shared dispatch task
     * (CONFIG_ESP_TIMER_TASK_STACK_SIZE, only 3584 bytes - shared with
     * EVERY OTHER esp_timer callback in this firmware: telemetry sampling,
     * profile curve follower, session snapshot, watchdogs, etc.). profile/
     * segs/json below are `static` (NOT stack-allocated) - together
     * they're ~2.5KB, which blew the shared 3584-byte stack and caused a
     * `stack overflow in task esp_timer` crash/reboot loop when this was
     * first added as plain stack locals. Safe as `static` because esp_timer
     * callbacks are strictly serialized on that one task (never reentrant/
     * concurrent), so there's no risk of two calls overlapping on the same
     * buffers. */
    bool curve_active = (snap.phase == ROAST_PHASE_ROASTING || snap.phase == ROAST_PHASE_DEVELOPMENT ||
                          snap.phase == ROAST_PHASE_COOLING);
    static roast_profile_t profile;
    bool has_profile = (profile_store_get_selected(&profile) == ESP_OK);
    bool has_target = false;
    float ttemp = 0.0f;
    int tfan = 0;
    /* TBT should show during PREHEAT too (operator request: preheat now
     * actually heats toward the first setpoint's target - see
     * profile_curve_follower.c) - held flat at segment 0's target since
     * PREHEAT has no CHARGE-relative elapsed-time reference yet. */
    if ((curve_active || snap.phase == ROAST_PHASE_PREHEAT) && has_profile) {
        uint32_t elapsed_s = curve_active ? (uint32_t)(snap.elapsed_ms / 1000) : 0;
        ttemp = roast_profile_get_target_temp_c(&profile, elapsed_s);
        tfan = roast_profile_get_target_fan_pct(&profile, elapsed_s);
        has_target = true;
    }

    static char segs[900];
    int segs_len = snprintf(segs, sizeof(segs), "[");
    if (has_profile) {
        for (uint8_t i = 0; i < profile.point_count && segs_len < (int)sizeof(segs) - 40; i++) {
            segs_len += snprintf(segs + segs_len, sizeof(segs) - segs_len, "%s[%lu,%.1f,%u]", i == 0 ? "" : ",",
                                  (unsigned long)profile.points[i].duration_s,
                                  (double)profile.points[i].target_temp_c, (unsigned)profile.points[i].target_fan_pct);
        }
    }
    snprintf(segs + segs_len, sizeof(segs) - segs_len, "]");
    uint32_t duration_s = has_profile ? roast_profile_total_duration_s(&profile) : 0;

    /* Marked-event markers (Turning Point/FC/SC/Cool Start) for the web
     * chart's vertical dashed reference lines - mirrors the on-device
     * dashboard's rebuild_event_markers(). roast_events_get_all() is a
     * cheap in-RAM copy (max ROAST_EVENTS_MAX=16 entries), sent on every
     * tick same as `segs` above so a newly-marked event (manual or
     * automatic) shows up on the web chart within one broadcast period. */
    roast_event_record_t events[ROAST_EVENTS_MAX];
    size_t event_count = roast_events_get_all(events, ROAST_EVENTS_MAX);
    static char events_json[420];
    int events_len = snprintf(events_json, sizeof(events_json), "[");
    for (size_t i = 0; i < event_count && events_len < (int)sizeof(events_json) - 24; i++) {
        events_len += snprintf(events_json + events_len, sizeof(events_json) - events_len, "%s[%lld,%d]",
                                i == 0 ? "" : ",", (long long)events[i].elapsed_ms, (int)events[i].type);
    }
    snprintf(events_json + events_len, sizeof(events_json) - events_len, "]");

    static char json[2200]; /* generous margin above segs(900)+events_json(420)+everything else - see the format-truncation lesson elsewhere in this project */
    snprintf(json, sizeof(json),
             "{\"phase\":\"%s\",\"mode\":\"%s\",\"paused\":%s,\"elapsedMs\":%lld,"
             "\"bt\":%.1f,\"sensorValid\":%s,\"ror\":%.1f,\"dtr\":%.1f,\"fan\":%d,\"heater\":%d,"
             "\"hasTarget\":%s,\"ttemp\":%.1f,\"tfan\":%d,\"segs\":%s,\"durS\":%lu,\"events\":%s,"
             "\"alarmText\":\"%s\",\"alarmNeedsAck\":%s}",
             phase_str(snap.phase), session->control_mode == ROAST_MODE_PROFILE ? "PROFILE" : "MANUAL",
             snap.paused ? "true" : "false", (long long)snap.elapsed_ms,
             snap.bean_temp_c, snap.sensor_valid ? "true" : "false", snap.ror_c_per_min, snap.dtr_pct,
             snap.fan_pct, snap.heater_pct, has_target ? "true" : "false", ttemp, tfan, segs,
             (unsigned long)duration_s, events_json, needs_ack ? alarm_str(alarm) : "", needs_ack ? "true" : "false");

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)json;
    frame.len = strlen(json);
    /* memset() left frame.final = false ("more fragments follow") - every
     * broadcast was therefore an invalid, never-completed WS message. Found
     * via a live browser test: `WebSocket connection ... failed: Could not
     * decode a text frame as UTF-8` - the browser kept dropping/reopening
     * the connection before any telemetry could accumulate on the chart,
     * which is what made the solid actual-value lines never appear. Each
     * broadcast is a complete, standalone message, so this must be true. */
    frame.final = true;

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == 0) {
            continue;
        }
        esp_err_t err = httpd_ws_send_frame_async(s_server_handle, s_ws_fds[i], &frame);
        if (err != ESP_OK) {
            s_ws_fds[i] = 0; /* Client gone - prune. */
        }
    }
}

/* ---- T042: web control command endpoint ---- */

static esp_err_t control_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= CONTROL_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    char body[CONTROL_BODY_MAX_LEN];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char action[32] = {0};
    int value = 0;
    sscanf(body, "action=%31[^&]", action);
    char *value_pos = strstr(body, "value=");
    if (value_pos != NULL) {
        value = atoi(value_pos + strlen("value="));
    }

    esp_err_t err;
    if (strcmp(action, "set_fan") == 0) {
        err = command_dispatcher_set_fan_pct((uint8_t)value, SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "set_heater") == 0) {
        err = command_dispatcher_set_heater_pct((uint8_t)value, SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "pause") == 0) {
        err = command_dispatcher_pause_session(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "resume") == 0) {
        err = command_dispatcher_resume_session(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "cancel") == 0) {
        err = command_dispatcher_cancel_session(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "emergency_stop") == 0) {
        err = command_dispatcher_emergency_stop(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "ack_alarm") == 0) {
        err = command_dispatcher_acknowledge_alarm(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "confirm_charge") == 0) {
        err = command_dispatcher_confirm_charge(SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "switch_manual") == 0) {
        /* T046: irreversible for the session - the web UI must have already
         * gotten an explicit operator confirmation (JS confirm() dialog)
         * before posting this, same as the `value=1` "confirmed" flag. */
        err = command_dispatcher_switch_to_manual_artisan(value == 1, SAFETY_CMD_SOURCE_WEB);
    } else if (strcmp(action, "start") == 0) {
        /* Same rule as the display's Start Roast button (T034): a selected
         * preset always runs as a Profile-mode session; otherwise plain
         * Manual/Artisan. */
        roast_profile_t profile;
        bool has_profile = (profile_store_get_selected(&profile) == ESP_OK);
        err = session_sm_start(has_profile ? ROAST_MODE_PROFILE : ROAST_MODE_MANUAL_ARTISAN);
        if (err == ESP_OK) {
            roast_telemetry_service_on_roast_started();
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
        return ESP_FAIL;
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Sends the currently selected preset's full segment curve as a small
 * embedded <script> (duration_s/target_temp_c/target_fan_pct per point) so
 * the client can compute the WHOLE dashed target curve immediately at page
 * load - matching the on-device display, which shows the full profile
 * curve right away too, not just the portion elapsed so far. The selected
 * preset can no longer change mid-roast (profile_store_set_selected()
 * refuses that), so this stays valid for the page's whole lifetime; the
 * operator needs to reload the page to pick up a DIFFERENT preset picked
 * elsewhere while idle (acceptable - same as any other static page load). */
static void send_profile_curve_script(httpd_req_t *req)
{
    roast_profile_t profile;
    bool has_profile = (profile_store_get_selected(&profile) == ESP_OK);

    httpd_resp_send_chunk(req, "<script>var profileSegments=[", HTTPD_RESP_USE_STRLEN);
    if (has_profile) {
        for (uint8_t i = 0; i < profile.point_count; i++) {
            char seg[64];
            snprintf(seg, sizeof(seg), "%s{\"d\":%lu,\"t\":%.1f,\"f\":%u}", i == 0 ? "" : ",",
                     (unsigned long)profile.points[i].duration_s, (double)profile.points[i].target_temp_c,
                     (unsigned)profile.points[i].target_fan_pct);
            httpd_resp_send_chunk(req, seg, HTTPD_RESP_USE_STRLEN);
        }
    }
    char tail[48];
    snprintf(tail, sizeof(tail), "];var profileDurationS=%lu;</script>",
             (unsigned long)(has_profile ? roast_profile_total_duration_s(&profile) : 0));
    httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
}

/* ---- T043/T044/T045: dashboard page (chart + controls + E-Stop + alarm banner) ---- */

static const char *DASHBOARD_SCRIPT =
    "<script>"
    "(function(){"
    /* Fixed elapsed-time-indexed timeline spanning the WHOLE preset
     * duration (matching the on-device display) instead of a rolling
     * window: the dashed target curve is computed ONCE from
     * profileSegments/profileDurationS (embedded by
     * send_profile_curve_script(), see dashboard_routes.c) and shows the
     * complete preset immediately, while the solid actual-BT array starts
     * all-null and fills in index-by-index (idx = elapsedS *
     * (MAXPTS-1) / durationS) as telemetry arrives during the roast.
     * There's no solid "actual" Fan line - this hardware has no RPM sensor
     * to independently measure it, so fanData would just be an echo of
     * whatever was last commanded (already shown by the dashed Fan target
     * line, and redundant/misleading as a second "measured" line). */
    "var MAXPTS=150;"
    "var durationS=(typeof profileDurationS!=='undefined'&&profileDurationS>0)?profileDurationS:1200;"
    "var lastSegsJson='';"
    "var btData=new Array(MAXPTS).fill(null);"
    "var ttempData=new Array(MAXPTS).fill(null);"
    "var tfanData=new Array(MAXPTS).fill(null);"
    "var eventMarkers=[];"
    "var EVENT_LABELS=['TP','FCs','FCe','SCs','SCe','Cool'];"
    "function computeTargetCurve(){"
    "if(typeof profileSegments==='undefined'||profileSegments.length===0){"
    "for(var i=0;i<MAXPTS;i++){ttempData[i]=null;tfanData[i]=null;}return;}"
    "for(var i=0;i<MAXPTS;i++){"
    "var t=i*durationS/(MAXPTS-1);var cursor=0,temp=0,fan=0;"
    "for(var s=0;s<profileSegments.length;s++){var seg=profileSegments[s];"
    "if(t<cursor+seg.d||s===profileSegments.length-1){temp=seg.t;fan=seg.f;break;}"
    "cursor+=seg.d;}"
    "ttempData[i]=temp;tfanData[i]=fan;}"
    "}"
    "computeTargetCurve();"
    "var chart=document.getElementById('chart');var ctx=chart.getContext('2d');"
    "function fmtTime(ms){var s=Math.floor(ms/1000);var m=Math.floor(s/60);s=s%60;"
    "return (m<10?'0':'')+m+':'+(s<10?'0':'')+s;}"
    "function setText(id,t){var el=document.getElementById(id);if(el)el.textContent=t;}"
    /* PAD_TOP/PAD_BOTTOM keep the plotted line from touching the canvas
     * edges (operator-reported: "nao tem margem em cima e embaixo, a linha
     * fica colada"). mapY() is the single source of truth for value->pixel
     * mapping so plot()/drawSegmentLabels()/drawEventMarkers() all agree. */
    "var PAD_TOP=14,PAD_BOTTOM=14;"
    "function mapY(value,max){return PAD_TOP+(1-value/max)*(h-PAD_TOP-PAD_BOTTOM);}"
    "function plot(data,max,color,dashed){"
    "ctx.setLineDash(dashed?[5,5]:[]);ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
    "var started=false;"
    "for(var i=0;i<data.length;i++){"
    /* Skip nulls WITHOUT resetting `started` - operator-reported: the solid
     * line looked like disconnected dots because the coarse ~10s-spaced
     * backfill (and any brief sensor-invalid tick) leaves gaps in the
     * array; the old code treated every null as "break the line", so each
     * isolated real value rendered as its own unconnected dot instead of
     * one continuous curve. Just skipping (not breaking) bridges any gap
     * with a straight line to the next real point - nothing is drawn PAST
     * the last real point, since the loop simply ends. */
    "if(data[i]===null||data[i]===undefined){continue;}"
    "var x=w*i/(MAXPTS-1),y=mapY(data[i],max);"
    "if(!started){ctx.moveTo(x,y);started=true;}else{ctx.lineTo(x,y);}}"
    "ctx.stroke();ctx.setLineDash([]);"
    "}"
    /* Numeric value labels at each segment's own setpoint (matching the
     * on-device display's chart) - one small number per segment for temp
     * (orange, above the line) and fan (green, below the line), centered
     * over that segment's midpoint. */
    "function drawSegmentLabels(){"
    "if(typeof profileSegments==='undefined'||!profileSegments||profileSegments.length===0)return;"
    "ctx.font='11px sans-serif';ctx.setLineDash([]);"
    "var cursor=0;"
    "for(var s=0;s<profileSegments.length;s++){"
    "var seg=profileSegments[s];var mid=cursor+seg.d/2;cursor+=seg.d;"
    "var x=w*mid/durationS;"
    "var ty=mapY(seg.t,260);var fy=mapY(seg.f,100);"
    "ctx.fillStyle='#FF9746';ctx.fillText(seg.t.toFixed(0),x-8,ty-6);"
    "ctx.fillStyle='#66BB6A';ctx.fillText(seg.f+'%',x-8,fy+14);"
    "}"
    "}"
    /* Vertical dashed reference lines for marked roast events (Turning
     * Point/First&Second Crack/Cool Start), matching the on-device
     * dashboard - previously only implemented there. */
    "function drawEventMarkers(){"
    "ctx.font='10px sans-serif';"
    "for(var i=0;i<eventMarkers.length;i++){"
    "var ev=eventMarkers[i];"
    "var x=w*(ev.t/1000)/durationS;"
    "if(x<0)x=0;if(x>w)x=w;"
    "ctx.setLineDash([3,3]);ctx.strokeStyle='#BA68C8';ctx.lineWidth=1;"
    "ctx.beginPath();ctx.moveTo(x,PAD_TOP);ctx.lineTo(x,h-PAD_BOTTOM);ctx.stroke();"
    "ctx.setLineDash([]);ctx.fillStyle='#BA68C8';"
    "ctx.fillText(EVENT_LABELS[ev.type]||'?',x+2,PAD_TOP+10);"
    "}"
    "}"
    "var w,h;"
    "function draw(){"
    "chart.width=chart.clientWidth;chart.height=260;"
    "w=chart.width;h=chart.height;ctx.clearRect(0,0,w,h);"
    "ctx.strokeStyle='#333';ctx.lineWidth=1;ctx.beginPath();"
    "for(var i=1;i<4;i++){var y=PAD_TOP+(h-PAD_TOP-PAD_BOTTOM)*i/4;ctx.moveTo(0,y);ctx.lineTo(w,y);}ctx.stroke();"
    "plot(ttempData,260,'#FF9746',true);plot(tfanData,100,'#66BB6A',true);"
    "plot(btData,260,'#FF9746',false);"
    "drawSegmentLabels();"
    "drawEventMarkers();"
    "}"
    "function onMessage(ev){"
    "var d;try{d=JSON.parse(ev.data);}catch(e){return;}"
    "if(d.segs){"
    "var segsJson=JSON.stringify(d.segs);"
    "if(segsJson!==lastSegsJson){"
    "lastSegsJson=segsJson;"
    "profileSegments=d.segs.map(function(s){return {d:s[0],t:s[1],f:s[2]};});"
    "durationS=(d.durS>0)?d.durS:1200;"
    "computeTargetCurve();"
    "}"
    "}"
    "if(d.events){eventMarkers=d.events.map(function(e){return {t:e[0],type:e[1]};});}"
    "setText('phase',d.phase+(d.paused?' (PAUSED)':''));"
    "setText('mode',d.mode);"
    "setText('timer',fmtTime(d.elapsedMs));"
    "setText('bt',d.sensorValid?d.bt.toFixed(1)+' C':'--');"
    "setText('tbt',(d.hasTarget?d.ttemp.toFixed(1)+' C':'--'));"
    "setText('ror',d.ror.toFixed(1)+' C/min');"
    "setText('dtr',d.dtr>=0?d.dtr.toFixed(0)+'%':'--');"
    "setText('fan',d.fan+'%');"
    "setText('heater',d.heater+'%');"
    "var active=(d.phase==='ROASTING'||d.phase==='DEVELOPMENT'||d.phase==='COOLING');"
    "if(active){"
    "var idx=Math.floor((d.elapsedMs/1000)*(MAXPTS-1)/durationS);"
    "if(idx<0)idx=0;if(idx>=MAXPTS)idx=MAXPTS-1;"
    "if(d.sensorValid)btData[idx]=d.bt;"
    "}"
    "draw();"
    "var banner=document.getElementById('alarmBanner');"
    "if(d.alarmNeedsAck){banner.style.display='flex';setText('alarmText',d.alarmText);}"
    "else{banner.style.display='none';}"
    "var idleLike=(d.phase==='IDLE'||d.phase==='COMPLETED'||d.phase==='ABORTED');"
    "var preheat=(d.phase==='PREHEAT');"
    "document.getElementById('startBtn').style.display=idleLike?'inline-block':'none';"
    "document.getElementById('chargeBtn').style.display=preheat?'inline-block':'none';"
    "document.getElementById('activeControls').style.display=(!idleLike&&!preheat)?'inline-flex':'none';"
    "document.getElementById('pauseBtn').textContent=d.paused?'Resume':'Pause';"
    "document.getElementById('switchManualBtn').style.display=(!idleLike&&d.mode==='PROFILE')?'inline-block':'none';"
    "}"
    "function connect(){"
    "var proto=(location.protocol==='https:')?'wss:':'ws:';"
    "var ws=new WebSocket(proto+'//'+location.host+'/ws/telemetry');"
    "ws.onmessage=onMessage;"
    "ws.onclose=function(){setTimeout(connect,2000);};"
    "ws.onerror=function(){ws.close();};"
    "}"
    /* Operator-reported bug fix: opening/refreshing the dashboard mid-roast
     * used to lose every point recorded before the page connected, since
     * btData always started out empty and only filled in as new WebSocket
     * messages arrived. Backfill from the already-recorded portion of the
     * active session (GET /api/dashboard/live_history) so a fresh page
     * load shows the whole curve so far immediately instead of just a
     * fragment. IMPORTANT: this runs FULLY IN PARALLEL with connect() (not
     * chained/gated behind it) - an earlier version called connect() only
     * AFTER the fetch settled, which needlessly delayed the start of LIVE
     * telemetry by however long the historical fetch took (a real,
     * reported regression: "ainda demora bastante para o grafico abrir"). */
    "connect();"
    "fetch('/api/dashboard/live_history').then(function(r){return r.json();}).then(function(data){"
    "var points=data.pts||[];"
    "for(var i=0;i<points.length;i++){"
    "var t=points[i][0],bt=points[i][1];"
    "var idx=Math.floor((t/1000)*(MAXPTS-1)/durationS);"
    "if(idx<0)idx=0;if(idx>=MAXPTS)idx=MAXPTS-1;"
    "btData[idx]=bt;"
    "}"
    "if(data.events){eventMarkers=data.events.map(function(e){return {t:e[0],type:e[1]};});}"
    "draw();"
    "}).catch(function(){});"
    "draw();"
    "function post(action,value){"
    "var body='action='+encodeURIComponent(action)+(value!==undefined?'&value='+encodeURIComponent(value):'');"
    "fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
    "}"
    "document.getElementById('startBtn').addEventListener('click',function(){post('start');"
    "btData=new Array(MAXPTS).fill(null);draw();});"
    "document.getElementById('chargeBtn').addEventListener('click',function(){post('confirm_charge');});"
    "document.getElementById('pauseBtn').addEventListener('click',function(){"
    "post(this.textContent==='Pause'?'pause':'resume');});"
    "document.getElementById('cancelBtn').addEventListener('click',function(){post('cancel');});"
    "document.getElementById('estopBtn').addEventListener('click',function(){"
    "if(confirm('Emergency Stop - are you sure?'))post('emergency_stop');});"
    "document.getElementById('ackBtn').addEventListener('click',function(){post('ack_alarm');});"
    "document.getElementById('switchManualBtn').addEventListener('click',function(){"
    "if(confirm('Switch to Manual/Artisan mode? This cannot be undone for this roast.'))post('switch_manual',1);});"
    "var fanSlider=document.getElementById('fanSlider');"
    "fanSlider.addEventListener('input',function(){setText('fanSliderVal',this.value+'%');});"
    "fanSlider.addEventListener('change',function(){post('set_fan',this.value);});"
    "var heaterSlider=document.getElementById('heaterSlider');"
    "heaterSlider.addEventListener('input',function(){setText('heaterSliderVal',this.value+'%');});"
    "heaterSlider.addEventListener('change',function(){post('set_heater',this.value);});"
    "window.addEventListener('resize',draw);"
    "})();"
    "</script>";

void dashboard_routes_send_page(httpd_req_t *req)
{
    /* Sent as separate chunks instead of one big snprintf'd buffer - the
     * full page (style + markup + script) is several KB, comfortably over
     * a reasonable stack buffer size (and -Werror=format-truncation
     * correctly refuses to let a single fixed-size snprintf silently
     * truncate something this large). Each piece here is already a
     * complete, static, null-terminated string, so plain chunked sends
     * avoid needing a scratch buffer (or dynamic formatting) at all. */
    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "dashboard");
    httpd_resp_send_chunk(req, "<main class='content'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<div id='alarmBanner'><span id='alarmText'></span><button id='ackBtn'>ACK</button></div>"
                           "<div class='card'>"
                           "<h1>Roast Dashboard</h1>"
                           "<div class='sub'>Phase: <span id='phase'>--</span> &middot; Mode: <span id='mode'>--</span>"
                           " &middot; Time: <span id='timer'>00:00</span></div>"
                           "<canvas id='chart'></canvas>"
                           "<div class='legend'><span class='bt'>&#9679; BT</span> &nbsp; "
                           "<span style='color:var(--muted)'>(dashed = profile target, incl. Fan - no solid Fan line, this hardware has no RPM sensor)</span></div>"
                           "<div class='grid'>"
                           "<div class='stat'><div class='label'>Bean Temp / Target</div><div class='value'><span id='bt'>--</span> / <span id='tbt'>--</span></div></div>"
                           "<div class='stat'><div class='label'>Rate of Rise</div><div class='value' id='ror'>--</div></div>"
                           "<div class='stat'><div class='label'>DTR%</div><div class='value' id='dtr'>--</div></div>"
                           "<div class='stat'><div class='label'>Fan / Heater</div><div class='value'><span id='fan'>0%</span> / <span id='heater'>0%</span></div></div>"
                           "</div>"
                           "<div class='sliderrow'><div class='label'><span>Fan override</span><span id='fanSliderVal'>--</span></div>"
                           "<input type='range' id='fanSlider' min='0' max='100' step='5' value='0'></div>"
                           "<div class='sliderrow'><div class='label'><span>Heater override</span><span id='heaterSliderVal'>--</span></div>"
                           "<input type='range' id='heaterSlider' min='0' max='100' step='5' value='0'></div>"
                           "<div class='btnrow'>"
                           "<button id='startBtn' class='primary'>Start Roast</button>"
                           "<button id='chargeBtn' class='primary' style='display:none'>Charge</button>"
                           "<span id='activeControls' style='display:none;gap:8px;'>"
                           "<button id='pauseBtn'>Pause</button>"
                           "<button id='cancelBtn' class='danger'>Cancel</button>"
                           "</span>"
                           "<button id='switchManualBtn' style='display:none' class='danger'>Switch to Manual</button>"
                           "<button id='estopBtn'>EMERGENCY STOP</button>"
                           "</div>"
                           "</div>",
                           HTTPD_RESP_USE_STRLEN);
    send_profile_curve_script(req);
    httpd_resp_send_chunk(req, DASHBOARD_SCRIPT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0); /* Terminates the chunked response. */
}

esp_err_t dashboard_routes_register(httpd_handle_t server)
{
    s_server_handle = server;
    memset(s_ws_fds, 0, sizeof(s_ws_fds));

    httpd_uri_t style_uri = { .uri = "/style.css", .method = HTTP_GET, .handler = style_css_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &style_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /style.css: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t ws_uri = {
        .uri = "/ws/telemetry",
        .method = HTTP_GET,
        .handler = ws_telemetry_handler,
        .is_websocket = true,
    };
    err = httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws/telemetry: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t control_uri = {
        .uri = "/api/control",
        .method = HTTP_POST,
        .handler = control_post_handler,
    };
    err = httpd_register_uri_handler(server, &control_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/control: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t live_history_uri = {
        .uri = "/api/dashboard/live_history",
        .method = HTTP_GET,
        .handler = live_history_get_handler,
    };
    err = httpd_register_uri_handler(server, &live_history_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/dashboard/live_history: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = ws_broadcast_timer_cb,
        .name = "ws_telemetry",
    };
    err = esp_timer_create(&timer_args, &s_broadcast_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_broadcast_timer, WS_BROADCAST_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Dashboard routes registered (/ws/telemetry, /api/control)");
    return ESP_OK;
}

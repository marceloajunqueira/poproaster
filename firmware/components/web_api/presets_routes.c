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
#define SAVE_BODY_MAX_LEN 2048
#define IMPORT_BODY_MAX_LEN 2048

/* Client-side segment-row template used both for the initial server-
 * rendered HEATING rows AND for rows added via "+ Add Segment" (JS
 * template literal in PRESET_EDIT_SCRIPT) - kept visually identical between
 * the two so a freshly-added row looks like any other. `%%` escapes a
 * literal '%' since this is used as an snprintf() format string.
 *
 * Per operator requirement: Cooling is no longer a per-segment toggle -
 * it's always exactly ONE mandatory trailing segment, rendered separately
 * (see the dedicated "coolDur" input in presets_edit_get_handler()) with
 * only its duration editable. These rows are ONLY ever heating segments. */
#define SEG_ROW_TEMPLATE \
    "<div class='row segrow' style='flex-wrap:wrap;gap:6px'>" \
    "<input class='sdur' type='number' value='%u' min='15' max='1800' step='15' style='width:70px'>s&nbsp;" \
    "<input class='stemp' type='number' value='%.1f' min='0' max='260' step='1' style='width:60px'>&#8451;&nbsp;" \
    "<input class='sfan' type='number' value='%u' min='0' max='100' step='5' style='width:55px'>%%fan&nbsp;" \
    "<button type='button' onclick='this.parentElement.remove()'>&times;</button>" \
    "</div>"

/* Vanilla JS (no external CDN, per FR-021) for the preset editor page:
 * addRow() mirrors SEG_ROW_TEMPLATE above (kept visually consistent);
 * savePreset() re-serializes EVERY .segrow element in current DOM order
 * with fresh sequential indices (so removing a row in the middle never
 * leaves a gap) and POSTs form-encoded to /api/presets/save; deletePreset()
 * POSTs to /api/presets/delete after a confirm() guard (destructive). */
#define PRESET_EDIT_SCRIPT \
    "<script>" \
    "function addRow(){" \
    "var rows=document.querySelectorAll('.segrow');" \
    "if(rows.length>=19){alert('Maximum 19 heating segments (plus the mandatory Cooling one).');return;}" \
    "var d=document.createElement('div');" \
    "d.className='row segrow';d.style.flexWrap='wrap';d.style.gap='6px';" \
    "d.innerHTML=`<input class='sdur' type='number' value='60' min='15' max='1800' step='15' style='width:70px'>s&nbsp;" \
    "<input class='stemp' type='number' value='200' min='0' max='260' step='1' style='width:60px'>&#8451;&nbsp;" \
    "<input class='sfan' type='number' value='60' min='0' max='100' step='5' style='width:55px'>%fan&nbsp;" \
    "<button type='button' onclick='this.parentElement.remove()'>&times;</button>`;" \
    /* Cooling's own row lives OUTSIDE #segList (a separate, fixed section -
     * see presets_edit_get_handler()), so appending to segList's end
     * naturally means "before Cooling" without any special-casing here. */ \
    "document.getElementById('segList').appendChild(d);" \
    "}" \
    "function savePreset(){" \
    "var rows=document.querySelectorAll('.segrow');" \
    "var name=document.getElementById('nameInput').value;" \
    "var coolDur=document.getElementById('coolDur').value;" \
    "var body='id='+encodeURIComponent(presetId)+'&name='+encodeURIComponent(name)+'&count='+(rows.length+1);" \
    "rows.forEach(function(r,i){" \
    "body+='&dur'+i+'='+r.querySelector('.sdur').value;" \
    "body+='&temp'+i+'='+r.querySelector('.stemp').value;" \
    "body+='&fan'+i+'='+r.querySelector('.sfan').value;" \
    "});" \
    /* Mandatory trailing Cooling segment - always the LAST index; the
     * server forces is_cooling=true and the fixed temp/fan values for it
     * regardless of what (if anything) is submitted for temp/fan there. */ \
    "body+='&dur'+rows.length+'='+coolDur;" \
    "fetch('/api/presets/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})" \
    ".then(function(r){return r.text().then(function(t){return {ok:r.ok,text:t};});})" \
    ".then(function(res){if(res.ok){location.href='/presets';}else{document.getElementById('editStatus').textContent='Error: '+res.text;}});" \
    "}" \
    "function deletePreset(){" \
    "if(!confirm('Delete this profile? This cannot be undone.'))return;" \
    "fetch('/api/presets/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'id='+encodeURIComponent(presetId)})" \
    ".then(function(){location.href='/presets';});" \
    "}" \
    "</script>"

static void decode_percent_inplace(char *s)
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
}

static esp_err_t presets_list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Presets</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "presets");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'><h1>Presets</h1>"
                           "<p style='color:var(--muted);font-size:13px'>Tap a preset to select it for the next roast. "
                           "Use Edit to change its segments, or + New Profile to create one from scratch.</p>"
                           "<div class='btnrow'><a class='btnlink primary' href='/presets/edit?id=-1'>+ New Profile</a></div>",
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
    } else if (strcmp(error_val, "notfound") == 0) {
        httpd_resp_send_chunk(req, "<p style='color:var(--danger);font-weight:500'>Preset not found.</p>",
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
            char row[500];
            snprintf(row, sizeof(row),
                     "<div class='row'><span class='name%s'>%s%s</span>"
                     "<span><a class='btnlink' href='/presets/edit?id=%d'>Edit</a>&nbsp;"
                     "<a class='btnlink' href='/api/presets/export?id=%d'>Export</a>&nbsp;"
                     "<form method='POST' action='/api/presets/select' style='display:inline;margin:0'>"
                     "<input type='hidden' name='id' value='%d'><button type='submit'>%s</button></form></span></div>",
                     is_selected ? " selected" : "", entries[i].name, is_selected ? " (selected)" : "",
                     entries[i].id, entries[i].id, entries[i].id, is_selected ? "Selected" : "Select");
            httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        }
    }

    httpd_resp_send_chunk(req,
                           "<h2 style='margin-top:20px'>Import Preset</h2>"
                           "<p style='color:var(--muted);font-size:13px'>Upload a .json file previously exported from this page.</p>"
                           "<div class='sliderrow'><input type='file' id='importFile' accept='.json'></div>"
                           "<div class='btnrow'><button type='button' class='primary' onclick='importPreset()'>Import</button></div>"
                           "<p id='importStatus' class='sub'></p>"
                           "<script>"
                           "function importPreset(){"
                           "var f=document.getElementById('importFile').files[0];"
                           "if(!f){document.getElementById('importStatus').textContent='Choose a .json file first.';return;}"
                           "var reader=new FileReader();"
                           "reader.onload=function(){"
                           "fetch('/api/presets/import',{method:'POST',headers:{'Content-Type':'application/json'},body:reader.result})"
                           ".then(function(r){return r.text().then(function(t){return {ok:r.ok,text:t};});})"
                           ".then(function(res){if(res.ok){location.reload();}else{document.getElementById('importStatus').textContent='Error: '+res.text;}});"
                           "};"
                           "reader.readAsText(f);"
                           "}"
                           "</script>",
                           HTTPD_RESP_USE_STRLEN);

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

/* T032 web parity: create/edit a preset's segments from the browser (was
 * display-only until now). GET ?id=-1 starts a brand-new profile (a single
 * default heating segment plus the mandatory trailing Cooling one,
 * matching the display editor's own default); ?id=N loads an existing one. */
static esp_err_t presets_edit_get_handler(httpd_req_t *req)
{
    char query[32];
    char id_str[8] = {0};
    int id = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK && id_str[0] != '\0') {
            id = atoi(id_str);
        }
    }

    roast_profile_t profile;
    bool is_new = (id < 0);
    if (is_new) {
        memset(&profile, 0, sizeof(profile));
        strncpy(profile.name, "New Profile", sizeof(profile.name) - 1);
        profile.point_count = 1;
        profile.points[0].duration_s = 60;
        profile.points[0].target_temp_c = 200.0f;
        profile.points[0].target_fan_pct = 60;
        profile.points[0].is_cooling = false;
    } else if (profile_store_load(id, &profile) != ESP_OK) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/presets?error=notfound");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    /* Per operator requirement: Cooling is always exactly the last segment -
     * normalizes a freshly-seeded new profile (adds the default trailing
     * Cooling segment) and any legacy/imported profile that doesn't
     * already conform. Same helper the display's profile_editor.c calls. */
    roast_profile_ensure_trailing_cooling(&profile);

    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Edit Preset</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "presets");

    char header[512];
    snprintf(header, sizeof(header),
             "<main class='content'><div class='card'>"
             "<h1>%s Preset</h1>"
             "<label style='color:var(--muted);font-size:13px;display:block;margin:8px 0 4px'>Name</label>"
             "<input id='nameInput' type='text' value='%s' maxlength='31' style='width:100%%;padding:8px;"
             "border-radius:4px;border:1px solid #333;background:#2a2a2a;color:var(--on-surface);font-size:14px'>"
             "<h2 style='margin-top:16px'>Heating Segments</h2>"
             "<div id='segList'>",
             is_new ? "New" : "Edit", profile.name);
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);

    /* Every point EXCEPT the last (mandatory Cooling, rendered separately
     * below) is a plain heating row - no more per-segment Cooling toggle. */
    for (uint8_t i = 0; i + 1 < profile.point_count; i++) {
        char row[900];
        const roast_profile_point_t *pt = &profile.points[i];
        snprintf(row, sizeof(row), SEG_ROW_TEMPLATE, (unsigned)pt->duration_s, (double)pt->target_temp_c,
                 (unsigned)pt->target_fan_pct);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<div class='btnrow'><button type='button' onclick='addRow()'>+ Add Segment</button></div>",
                           HTTPD_RESP_USE_STRLEN);

    /* Mandatory trailing Cooling segment - not a toggle anymore, always
     * present, only its duration is operator-editable (fixed 0C/100% fan,
     * per roast_profile.h). */
    char cooling_html[400]; /* the fixed literal HTML alone is ~320 bytes - 300 was too tight (format-truncation build error) */
    snprintf(cooling_html, sizeof(cooling_html),
             "<h2 style='margin-top:16px'>Cooling (mandatory)</h2>"
             "<div class='row' style='flex-wrap:wrap;gap:6px'>"
             "<input id='coolDur' type='number' value='%u' min='15' max='1800' step='15' style='width:70px'>s&nbsp;"
             "<span style='color:var(--muted);font-size:13px'>Fixed: 0&#8451;, 100%% fan - always runs last</span>"
             "</div>",
             (unsigned)profile.points[profile.point_count - 1].duration_s);
    httpd_resp_send_chunk(req, cooling_html, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
                           is_new
                               ? "<div class='btnrow'><button type='button' class='primary' onclick='savePreset()'>Save</button>"
                                 "<a class='btnlink' href='/presets'>Cancel</a></div>"
                               : "<div class='btnrow'><button type='button' class='primary' onclick='savePreset()'>Save</button>"
                                 "<button type='button' class='danger' onclick='deletePreset()'>Delete Profile</button>"
                                 "<a class='btnlink' href='/presets'>Cancel</a></div>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "<p id='editStatus' class='sub'></p></div></main></div>", HTTPD_RESP_USE_STRLEN);

    char id_script[48];
    snprintf(id_script, sizeof(id_script), "<script>var presetId=%d;</script>", id);
    httpd_resp_send_chunk(req, id_script, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, PRESET_EDIT_SCRIPT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t presets_save_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= SAVE_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    /* Static, not stack-local: this can be a couple KB for a full 20-segment
     * profile - see this project's general convention of keeping sizeable
     * scratch buffers off the stack. */
    static char body[SAVE_BODY_MAX_LEN];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    int id = -1;
    char id_str[8] = {0};
    if (httpd_query_key_value(body, "id", id_str, sizeof(id_str)) == ESP_OK && id_str[0] != '\0') {
        id = atoi(id_str);
    }

    char name_raw[40] = {0};
    httpd_query_key_value(body, "name", name_raw, sizeof(name_raw));
    decode_percent_inplace(name_raw);

    int count = 1;
    char count_str[8] = {0};
    if (httpd_query_key_value(body, "count", count_str, sizeof(count_str)) == ESP_OK) {
        count = atoi(count_str);
    }
    if (count < 1) {
        count = 1;
    }
    if (count > ROAST_PROFILE_MAX_POINTS) {
        count = ROAST_PROFILE_MAX_POINTS;
    }

    roast_profile_t profile = {0};
    strncpy(profile.name, name_raw[0] ? name_raw : "Untitled", sizeof(profile.name) - 1);
    profile.point_count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        char key[16];
        char val[16];

        int dur = 60;
        snprintf(key, sizeof(key), "dur%d", i);
        if (httpd_query_key_value(body, key, val, sizeof(val)) == ESP_OK) {
            dur = atoi(val);
        }
        if (dur < 15) {
            dur = 15;
        }
        if (dur > 1800) {
            dur = 1800;
        }

        float temp = 200.0f;
        snprintf(key, sizeof(key), "temp%d", i);
        if (httpd_query_key_value(body, key, val, sizeof(val)) == ESP_OK) {
            temp = (float)atof(val);
        }
        if (temp < 0.0f) {
            temp = 0.0f;
        }
        if (temp > 260.0f) {
            temp = 260.0f;
        }

        int fan = 60;
        snprintf(key, sizeof(key), "fan%d", i);
        if (httpd_query_key_value(body, key, val, sizeof(val)) == ESP_OK) {
            fan = atoi(val);
        }
        if (fan < 0) {
            fan = 0;
        }
        if (fan > 100) {
            fan = 100;
        }

        /* Per operator requirement: Cooling is no longer a per-segment
         * choice submitted by the client - it's always exactly the LAST
         * index (the client's savePreset() JS always appends its
         * dedicated "coolDur" duration as the final segment - see
         * PRESET_EDIT_SCRIPT). Any "cool<i>" field is intentionally no
         * longer read/trusted here. */
        bool cooling = (i == count - 1);

        roast_profile_point_t *pt = &profile.points[i];
        pt->duration_s = (uint32_t)dur;
        pt->is_cooling = cooling;
        if (cooling) {
            /* Not operator-editable - roast_profile.h's fixed Cooling values. */
            pt->target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
            pt->target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
        } else {
            if (fan < ROAST_PROFILE_FAN_MIN_PCT) {
                fan = ROAST_PROFILE_FAN_MIN_PCT; /* FR-004 fan floor, same clamp as the display editor. */
            }
            pt->target_temp_c = temp;
            pt->target_fan_pct = (uint8_t)fan;
        }
    }
    roast_profile_ensure_trailing_cooling(&profile); /* Defensive - should already conform given the above. */

    esp_err_t err;
    if (id < 0) {
        int new_id = -1;
        err = profile_store_create(&profile, &new_id);
    } else {
        err = profile_store_update(id, &profile);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save preset failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save profile");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t presets_delete_post_handler(httpd_req_t *req)
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
    esp_err_t err = profile_store_delete(atoi(id_str));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Delete preset failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete profile");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* T039: exports a stored profile as JSON - a compact array-of-tuples format
 * (same convention as dashboard_routes.c's WS "segs" field:
 * [duration_s, target_temp_c, target_fan_pct, is_cooling]) rather than a
 * verbose object-per-point format, so the T040 import parser below can stay
 * a simple hand-written scanner (no cJSON dependency, matching this whole
 * project's manual-parsing convention) instead of a general JSON parser. */
static esp_err_t presets_export_get_handler(httpd_req_t *req)
{
    char query[32];
    char id_str[8] = {0};
    int id = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id_str, sizeof(id_str));
        id = atoi(id_str);
    }

    roast_profile_t profile;
    if (profile_store_load(id, &profile) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Profile not found");
        return ESP_FAIL;
    }

    char filename_header[80];
    snprintf(filename_header, sizeof(filename_header), "attachment; filename=\"%s.json\"", profile.name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", filename_header);

    static char json[1200]; /* static: comfortably larger than 20 points can produce - kept off the request handler's stack. */
    int len = snprintf(json, sizeof(json), "{\"name\":\"%s\",\"points\":[", profile.name);
    for (uint8_t i = 0; i < profile.point_count && len < (int)sizeof(json) - 40; i++) {
        const roast_profile_point_t *pt = &profile.points[i];
        len += snprintf(json + len, sizeof(json) - len, "%s[%lu,%.1f,%u,%d]", i == 0 ? "" : ",",
                         (unsigned long)pt->duration_s, (double)pt->target_temp_c,
                         (unsigned)pt->target_fan_pct, pt->is_cooling ? 1 : 0);
    }
    snprintf(json + len, sizeof(json) - len, "]}");

    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* T040: imports a profile previously exported by the handler above. Manual
 * scanner (not a general JSON parser): finds "name":"..." then "points":[
 * followed by a sequence of [dur,temp,fan,cooling] tuples up to the closing
 * "]}" - matches exactly (and only) the format presets_export_get_handler()
 * produces, same pragmatic scope as this project's other manual parsers
 * (form bodies, WS JSON, etc.). */
static esp_err_t presets_import_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= IMPORT_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    static char body[IMPORT_BODY_MAX_LEN];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    roast_profile_t profile = {0};
    strncpy(profile.name, "Imported Profile", sizeof(profile.name) - 1);

    const char *name_key = strstr(body, "\"name\":\"");
    if (name_key != NULL) {
        name_key += strlen("\"name\":\"");
        const char *name_end = strchr(name_key, '"');
        if (name_end != NULL) {
            size_t len = (size_t)(name_end - name_key);
            if (len >= sizeof(profile.name)) {
                len = sizeof(profile.name) - 1;
            }
            memcpy(profile.name, name_key, len);
            profile.name[len] = '\0';
        }
    }

    const char *points_key = strstr(body, "\"points\":[");
    if (points_key == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing points array");
        return ESP_FAIL;
    }
    const char *cursor = points_key + strlen("\"points\":[");

    uint8_t count = 0;
    while (count < ROAST_PROFILE_MAX_POINTS) {
        const char *bracket = strchr(cursor, '[');
        if (bracket == NULL) {
            break;
        }
        /* Stop if the array's closing ']' comes before the next point's '[' - end of points list. */
        const char *close_bracket = strchr(cursor, ']');
        if (close_bracket != NULL && close_bracket < bracket) {
            break;
        }

        unsigned long dur = 60;
        float temp = 200.0f;
        unsigned fan = 60;
        int cooling = 0;
        if (sscanf(bracket, "[%lu,%f,%u,%d]", &dur, &temp, &fan, &cooling) != 4) {
            break;
        }

        roast_profile_point_t *pt = &profile.points[count];
        pt->duration_s = (dur < 15) ? 15 : (dur > 1800 ? 1800 : dur);
        pt->is_cooling = (cooling != 0);
        if (pt->is_cooling) {
            pt->target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
            pt->target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
        } else {
            pt->target_temp_c = (temp < 0.0f) ? 0.0f : (temp > 260.0f ? 260.0f : temp);
            pt->target_fan_pct = (uint8_t)((fan < ROAST_PROFILE_FAN_MIN_PCT) ? ROAST_PROFILE_FAN_MIN_PCT : (fan > 100 ? 100 : fan));
        }
        count++;

        const char *next = strchr(bracket, ']');
        if (next == NULL) {
            break;
        }
        cursor = next + 1;
    }

    if (count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid segments found in file");
        return ESP_FAIL;
    }
    profile.point_count = count;
    /* Per operator requirement: Cooling is always exactly the last segment -
     * normalizes any imported file that predates/doesn't follow that rule
     * (e.g. no Cooling segment at all, or one that isn't last). */
    roast_profile_ensure_trailing_cooling(&profile);

    int new_id = -1;
    esp_err_t err = profile_store_create(&profile, &new_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Import failed to create profile: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create profile (storage full?)");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
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

    httpd_uri_t edit_uri = { .uri = "/presets/edit", .method = HTTP_GET, .handler = presets_edit_get_handler };
    err = httpd_register_uri_handler(server, &edit_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t save_uri = { .uri = "/api/presets/save", .method = HTTP_POST, .handler = presets_save_post_handler };
    err = httpd_register_uri_handler(server, &save_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t delete_uri = { .uri = "/api/presets/delete", .method = HTTP_POST, .handler = presets_delete_post_handler };
    err = httpd_register_uri_handler(server, &delete_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t export_uri = { .uri = "/api/presets/export", .method = HTTP_GET, .handler = presets_export_get_handler };
    err = httpd_register_uri_handler(server, &export_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t import_uri = { .uri = "/api/presets/import", .method = HTTP_POST, .handler = presets_import_post_handler };
    err = httpd_register_uri_handler(server, &import_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Presets routes registered (/presets, /presets/edit, /api/presets/select|save|delete)");
    return ESP_OK;
}


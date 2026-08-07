/**
 * @file presets_routes.c
 * @brief See header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"

#include "hal/fan_pwm.h"
#include "roast_core/profile_curve_follower.h"
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
#define SEG_FIELD_OPEN "<span style='display:inline-flex;align-items:center;gap:4px'>"
#define SEG_FIELD_CLOSE "</span>"
#define SEG_ROW_TEMPLATE \
    "<div class='row segrow' style='flex-wrap:wrap;justify-content:flex-start;gap:14px'>" \
    SEG_FIELD_OPEN "<input class='sdur' type='number' value='%u' min='30' max='1800' step='30' style='width:70px'>s" SEG_FIELD_CLOSE \
    SEG_FIELD_OPEN "<input class='stemp' type='number' value='%.1f' min='0' max='260' step='1' style='width:60px'>&#8451;" SEG_FIELD_CLOSE \
    SEG_FIELD_OPEN "<select class='sfan' style='width:110px'>%s</select>" SEG_FIELD_CLOSE \
    "<button type='button' onclick='this.parentElement.remove();if(window.updatePreviewChart)updatePreviewChart();'>&times;</button>" \
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
    "d.className='row segrow';d.style.flexWrap='wrap';d.style.justifyContent='flex-start';d.style.gap='14px';" \
    "d.innerHTML=\"<span style='display:inline-flex;align-items:center;gap:4px'><input class='sdur' type='number' value='60' min='30' max='1800' step='30' style='width:70px'>s</span>\"+" \
    "\"<span style='display:inline-flex;align-items:center;gap:4px'><input class='stemp' type='number' value='200' min='0' max='260' step='1' style='width:60px'>&#8451;</span>\"+" \
    "\"<span style='display:inline-flex;align-items:center;gap:4px'><select class='sfan' style='width:110px'>\"+FAN_OPTIONS_HTML+\"</select></span>\"+" \
    "\"<button type='button' onclick='this.parentElement.remove();if(window.updatePreviewChart)updatePreviewChart();'>&times;</button>\";" \
    /* Cooling's own row lives OUTSIDE #segList (a separate, fixed section -
     * see presets_edit_get_handler()), so appending to segList's end
     * naturally means "before Cooling" without any special-casing here. */ \
    "document.getElementById('segList').appendChild(d);" \
    "if(window.updatePreviewChart)updatePreviewChart();" \
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

/* Live-updating canvas preview of the temp/fan curve implied by the CURRENT
 * (unsaved) form values - lets the operator see the shape of the profile
 * while still editing, instead of only after Save+re-opening. Reads
 * .segrow/.sdur/.stemp/.sfan/#coolDur straight from the DOM on every
 * input/change event (same fields savePreset() itself reads), so it's
 * always showing exactly what would be saved right now. COOL_TEMP_C/
 * COOL_FAN_PCT are emitted as globals by presets_edit_get_handler() (from
 * the same roast_profile.h constants the server enforces on save), so the
 * Cooling segment's fixed values never drift out of sync with reality. */
#define PRESET_PREVIEW_SCRIPT \
    "<script>" \
    "(function(){" \
    "var canvas=document.getElementById('previewChart');if(!canvas)return;" \
    "var ctx=canvas.getContext('2d');" \
    "var PAD_TOP=14,PAD_BOTTOM=14;" \
    "function mapY(value,max,h){return PAD_TOP+(1-value/max)*(h-PAD_TOP-PAD_BOTTOM);}" \
    "function readSegments(){" \
    "var segs=[];" \
    "document.querySelectorAll('.segrow').forEach(function(r){" \
    "var d=parseFloat(r.querySelector('.sdur').value)||0;" \
    "var t=parseFloat(r.querySelector('.stemp').value)||0;" \
    "var f=parseFloat(r.querySelector('.sfan').value)||0;" \
    "segs.push({d:d,t:t,f:f});" \
    "});" \
    "var cd=document.getElementById('coolDur');" \
    "segs.push({d:cd?(parseFloat(cd.value)||0):0,t:COOL_TEMP_C,f:COOL_FAN_PCT});" \
    "return segs;" \
    "}" \
    "function drawPreview(){" \
    "var segs=readSegments();" \
    "var durationS=0;for(var i=0;i<segs.length;i++)durationS+=segs[i].d;" \
    "if(durationS<=0)durationS=1;" \
    "canvas.width=canvas.clientWidth;canvas.height=240;" \
    "var w=canvas.width,h=canvas.height;" \
    "ctx.clearRect(0,0,w,h);" \
    "ctx.strokeStyle='#333';ctx.lineWidth=1;ctx.beginPath();" \
    "for(var i=1;i<4;i++){var y=PAD_TOP+(h-PAD_TOP-PAD_BOTTOM)*i/4;ctx.moveTo(0,y);ctx.lineTo(w,y);}" \
    "ctx.stroke();" \
    "function plot(getVal,max,color){" \
    "ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();" \
    "var cursor=0,started=false;" \
    "for(var s=0;s<segs.length;s++){" \
    "var seg=segs[s];var x0=w*cursor/durationS;var x1=w*(cursor+seg.d)/durationS;" \
    "var y=mapY(getVal(seg),max,h);" \
    "if(!started){ctx.moveTo(x0,y);started=true;}else{ctx.lineTo(x0,y);}" \
    "ctx.lineTo(x1,y);cursor+=seg.d;" \
    "}" \
    "ctx.stroke();" \
    "}" \
    "plot(function(s){return s.t;},260,'#FF9746');" \
    "plot(function(s){return s.f;},100,'#66BB6A');" \
    "ctx.font='11px sans-serif';" \
    "var cursor=0;" \
    "for(var s=0;s<segs.length;s++){" \
    "var seg=segs[s];var mid=cursor+seg.d/2;cursor+=seg.d;" \
    "var x=w*mid/durationS;" \
    "var ty=mapY(seg.t,260,h);var fy=mapY(seg.f,100,h);" \
    "ctx.fillStyle='#FF9746';ctx.fillText(seg.t.toFixed(0),x-8,ty-6);" \
    "ctx.fillStyle='#66BB6A';ctx.fillText(seg.f+'%',x-8,fy+14);" \
    "}" \
    "var tm=Math.floor(durationS/60),ts=Math.floor(durationS%60);" \
    "var totalEl=document.getElementById('previewTotal');" \
    "if(totalEl)totalEl.textContent='Total: '+tm+':'+(ts<10?'0':'')+ts;" \
    "}" \
    "window.updatePreviewChart=drawPreview;" \
    "document.addEventListener('input',function(e){" \
    "if(!e.target||!e.target.classList)return;" \
    "var c=e.target.classList;" \
    "if(c.contains('sdur')||c.contains('stemp')||c.contains('sfan')||e.target.id==='coolDur')drawPreview();" \
    "});" \
    "document.addEventListener('change',function(e){" \
    "if(e.target&&e.target.classList&&e.target.classList.contains('sfan'))drawPreview();" \
    "});" \
    "window.addEventListener('resize',drawPreview);" \
    "drawPreview();" \
    "})();" \
    "</script>"

/* Builds the <option> list for a `.sfan` <select>, one per valid discrete
 * fan level (matching the L1/L2/L3 levels the on-device profile editor and
 * live dashboard already use - see hal/fan_pwm.h) - built from the HAL's
 * own level<->percent table so this can never drift out of sync with it
 * the way the old raw 0-100 percent <input> could. `current_pct` marks the
 * matching option `selected`; pass 0 (never a valid nonzero level's
 * percent) to leave none selected, so the browser defaults to the first
 * (lowest, floor) level - the correct default for a freshly-added segment. */
static void build_fan_select_html(char *buf, size_t buf_size, uint8_t current_pct)
{
    size_t used = 0;
    for (uint8_t level = 1; level <= FAN_PWM_LEVEL_MAX && used < buf_size; level++) {
        uint8_t pct = fan_pwm_level_to_pct(level);
        int n = snprintf(buf + used, buf_size - used, "<option value='%u'%s>L%u (%u%%)</option>",
                          (unsigned)pct, (pct == current_pct) ? " selected" : "", (unsigned)level, (unsigned)pct);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
    if (used < buf_size) {
        buf[used] = '\0';
    } else {
        buf[buf_size - 1] = '\0';
    }
}

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
        profile.points[0].target_fan_pct = ROAST_PROFILE_FAN_MIN_PCT;
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

    char header[640];
    snprintf(header, sizeof(header),
             "<main class='content' style='display:flex;gap:16px;align-items:flex-start;flex-wrap:wrap'>"
             "<div class='card' style='flex:1;min-width:320px'>"
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
        char fan_opts[160];
        const roast_profile_point_t *pt = &profile.points[i];
        build_fan_select_html(fan_opts, sizeof(fan_opts), pt->target_fan_pct);
        char row[1100];
        snprintf(row, sizeof(row), SEG_ROW_TEMPLATE, (unsigned)pt->duration_s, (double)pt->target_temp_c, fan_opts);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<div class='btnrow'><button type='button' onclick='addRow()'>+ Add Segment</button></div>",
                           HTTPD_RESP_USE_STRLEN);

    /* Mandatory trailing Cooling segment - not a toggle anymore, always
     * present, only its duration is operator-editable (fixed 0C/100% fan,
     * per roast_profile.h). */
    char cooling_html[420]; /* the fixed literal HTML alone is ~320 bytes - 300 was too tight (format-truncation build error) */
    snprintf(cooling_html, sizeof(cooling_html),
             "<h2 style='margin-top:16px'>Cooling (mandatory)</h2>"
             "<div class='row' style='flex-wrap:wrap;justify-content:flex-start;gap:14px'>"
             "<span style='display:inline-flex;align-items:center;gap:4px'>"
             "<input id='coolDur' type='number' value='%u' min='30' max='1800' step='30' style='width:70px'>s</span>"
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
    httpd_resp_send_chunk(req, "<p id='editStatus' class='sub'></p></div>", HTTPD_RESP_USE_STRLEN);

    /* Live preview card - fills the space next to the form (previously
     * empty on wider screens); see PRESET_PREVIEW_SCRIPT for the drawing
     * logic, which reads straight from this same form's fields. */
    httpd_resp_send_chunk(req,
                           "<div class='card' style='flex:1;min-width:320px'>"
                           "<h2>Live Preview</h2>"
                           "<canvas id='previewChart' style='width:100%;display:block'></canvas>"
                           "<p id='previewTotal' class='sub'></p>"
                           "</div>"
                           "</main></div>",
                           HTTPD_RESP_USE_STRLEN);

    char id_script[48];
    snprintf(id_script, sizeof(id_script), "<script>var presetId=%d;</script>", id);
    httpd_resp_send_chunk(req, id_script, HTTPD_RESP_USE_STRLEN);

    /* Shared option list for addRow()'s dynamically-created .sfan <select>s -
     * built from the same HAL table as the server-rendered rows above so
     * the two can never drift apart; current_pct=0 leaves nothing selected,
     * so the browser defaults a new row to the first (lowest floor) level. */
    char new_row_fan_opts[160];
    build_fan_select_html(new_row_fan_opts, sizeof(new_row_fan_opts), 0);
    char fan_opts_script[256];
    snprintf(fan_opts_script, sizeof(fan_opts_script), "<script>var FAN_OPTIONS_HTML=`%s`;</script>", new_row_fan_opts);
    httpd_resp_send_chunk(req, fan_opts_script, HTTPD_RESP_USE_STRLEN);

    char cooling_const_script[96];
    snprintf(cooling_const_script, sizeof(cooling_const_script), "<script>var COOL_TEMP_C=%.1f;var COOL_FAN_PCT=%u;</script>",
             (double)ROAST_PROFILE_COOLING_TEMP_C, (unsigned)ROAST_PROFILE_COOLING_FAN_PCT);
    httpd_resp_send_chunk(req, cooling_const_script, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, PRESET_EDIT_SCRIPT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, PRESET_PREVIEW_SCRIPT, HTTPD_RESP_USE_STRLEN);
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
        if (temp > MANUAL_TARGET_TEMP_MAX_C) {
            temp = MANUAL_TARGET_TEMP_MAX_C;
        }

        int fan = ROAST_PROFILE_FAN_MIN_PCT;
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
            /* Fan is quantized to 3 discrete levels (80/90/100%, see
             * hal/fan_pwm.h) - snap whatever the client sent to the nearest
             * one, never below Level 1, same rule as the display editor. */
            uint8_t fan_level = fan_pwm_pct_to_level((uint8_t)(fan > 255 ? 255 : fan));
            if (fan_level < 1) {
                fan_level = 1;
            }
            pt->target_temp_c = temp;
            pt->target_fan_pct = fan_pwm_level_to_pct(fan_level);
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
        unsigned fan = ROAST_PROFILE_FAN_MIN_PCT;
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
            pt->target_temp_c = (temp < 0.0f) ? 0.0f : (temp > MANUAL_TARGET_TEMP_MAX_C ? MANUAL_TARGET_TEMP_MAX_C : temp);
            /* Fan is quantized to 3 discrete levels (80/90/100%, see
             * hal/fan_pwm.h) - snap the imported value to the nearest one,
             * never below Level 1. */
            uint8_t fan_level = fan_pwm_pct_to_level((uint8_t)(fan > 255 ? 255 : fan));
            if (fan_level < 1) {
                fan_level = 1;
            }
            pt->target_fan_pct = fan_pwm_level_to_pct(fan_level);
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


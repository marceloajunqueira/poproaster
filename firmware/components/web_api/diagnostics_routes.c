/**
 * @file diagnostics_routes.c
 * @brief See header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "roast_core/pid_debug_log.h"
#include "roast_core/heater_pid.h"
#include "roast_core/pid_autotune.h"
#include "roast_core/profile_curve_follower.h"
#include "hal/touch_driver.h"
#include "web_api/diagnostics_routes.h"
#include "web_api/dashboard_routes.h"

static const char *TAG = "diagnostics_routes";

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External pin";
    case ESP_RST_SW: return "Software (esp_restart)";
    case ESP_RST_PANIC: return "Exception/panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog (stack overflow, deadlock, etc.)";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "Unknown";
    }
}

static const char *task_state_str(eTaskState state)
{
    switch (state) {
    case eRunning: return "Running";
    case eReady: return "Ready";
    case eBlocked: return "Blocked";
    case eSuspended: return "Suspended";
    case eDeleted: return "Deleted";
    default: return "Invalid";
    }
}

static void send_stat_row(httpd_req_t *req, const char *label, const char *value)
{
    char row[192];
    snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td></tr>", label, value);
    httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
}

/* Formats a byte count as "N KB" (rounded) - every heap/NVS figure here is
 * comfortably in the KB-MB range on this hardware, no need for B/MB/GB
 * unit-switching logic. */
static void fmt_kb(size_t bytes, char *out, size_t out_len)
{
    snprintf(out, out_len, "%u KB", (unsigned)((bytes + 512) / 1024));
}

static void send_heap_section(httpd_req_t *req, const char *title, uint32_t caps)
{
    char buf[128];
    char val[32];
    snprintf(buf, sizeof(buf), "<h2>%s</h2><table>", title);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    size_t total = info.total_free_bytes + info.total_allocated_bytes;

    fmt_kb(total, val, sizeof(val));
    send_stat_row(req, "Total", val);
    fmt_kb(info.total_allocated_bytes, val, sizeof(val));
    send_stat_row(req, "Used", val);
    fmt_kb(info.total_free_bytes, val, sizeof(val));
    send_stat_row(req, "Free", val);
    fmt_kb(info.largest_free_block, val, sizeof(val));
    send_stat_row(req, "Largest free block", val);
    fmt_kb(heap_caps_get_minimum_free_size(caps), val, sizeof(val));
    send_stat_row(req, "Lowest free ever (worst case)", val);

    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
}

static void send_system_section(httpd_req_t *req)
{
    httpd_resp_send_chunk(req, "<h2>System</h2><table>", HTTPD_RESP_USE_STRLEN);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s, rev v%d.%d, %d core(s)",
             chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "?", chip.revision / 100, chip.revision % 100, chip.cores);
    send_stat_row(req, "Chip", buf);

    send_stat_row(req, "IDF version", esp_get_idf_version());

    const esp_app_desc_t *app = esp_app_get_description();
    if (app != NULL) {
        snprintf(buf, sizeof(buf), "%s (built %s %s)", app->version, app->date, app->time);
        send_stat_row(req, "Firmware build", buf);
    }

    int64_t uptime_s = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%lldh %02lldm %02llds", (long long)(uptime_s / 3600),
             (long long)((uptime_s % 3600) / 60), (long long)(uptime_s % 60));
    send_stat_row(req, "Uptime", buf);

    send_stat_row(req, "Last reset reason", reset_reason_str(esp_reset_reason()));

    /* Operator-reported bug: touch occasionally goes dead mid-session until
     * power-cycled - see touch_driver.c's read_data recovery wrapper. A
     * nonzero count here confirms it happened (and self-recovered) without
     * needing a reboot. */
    char touch_buf[32];
    snprintf(touch_buf, sizeof(touch_buf), "%u", (unsigned)touch_driver_get_recovery_count());
    send_stat_row(req, "Touch (GT911) auto-recoveries", touch_buf);

    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
}

static void send_nvs_section(httpd_req_t *req)
{
    httpd_resp_send_chunk(req, "<h2>NVS (settings/profiles storage)</h2><table>", HTTPD_RESP_USE_STRLEN);

    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        char val[32];
        snprintf(val, sizeof(val), "%u", (unsigned)stats.total_entries);
        send_stat_row(req, "Total entries (slots)", val);
        snprintf(val, sizeof(val), "%u", (unsigned)stats.used_entries);
        send_stat_row(req, "Used entries", val);
        snprintf(val, sizeof(val), "%u", (unsigned)stats.free_entries);
        send_stat_row(req, "Free entries", val);
        snprintf(val, sizeof(val), "%u", (unsigned)stats.namespace_count);
        send_stat_row(req, "Namespaces", val);
    } else {
        send_stat_row(req, "Status", "Unavailable (NVS not initialized?)");
    }

    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
}

/* Lists every FreeRTOS task and its stack high-water-mark usage (how close
 * each one has ever come to overflowing) - directly answers "which task is
 * at risk" (see the esp_timer stack-overflow crash this was added after).
 * The scratch array is allocated from PSRAM (not the stack, not static
 * internal RAM) since it's a one-off diagnostic snapshot with no timing
 * sensitivity - keeps internal RAM free for everything else. */
static void send_tasks_section(httpd_req_t *req)
{
    httpd_resp_send_chunk(req,
                           "<h2>FreeRTOS tasks (stack usage)</h2>"
                           "<table><tr><th>Task</th><th>State</th><th>Prio</th>"
                           "<th>Stack free (low water)</th></tr>",
                           HTTPD_RESP_USE_STRLEN);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = heap_caps_malloc(task_count * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tasks == NULL) {
        tasks = malloc(task_count * sizeof(TaskStatus_t)); /* Fallback if PSRAM alloc fails for any reason. */
    }
    if (tasks != NULL) {
        uint32_t total_runtime = 0;
        UBaseType_t actual_count = uxTaskGetSystemState(tasks, task_count, &total_runtime);
        for (UBaseType_t i = 0; i < actual_count; i++) {
            char row[192];
            /* usStackHighWaterMark is in WORDS (4 bytes each on this arch),
             * not bytes - the classic FreeRTOS gotcha. */
            snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td><td>%u</td><td>%u bytes</td></tr>",
                     tasks[i].pcTaskName, task_state_str(tasks[i].eCurrentState),
                     (unsigned)tasks[i].uxCurrentPriority, (unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
            httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
        }
        free(tasks);
    } else {
        httpd_resp_send_chunk(req, "<tr><td colspan='4'>Could not allocate scratch buffer to list tasks</td></tr>",
                               HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
}

/* Operator-requested PID tuning aid: the log itself is written by
 * roast_core/pid_debug_log.c (every follower tick that drives a real
 * heater target, in both Profile and Manual mode) - this section just
 * shows its current size and offers download/clear actions. */
static void send_pid_log_section(httpd_req_t *req)
{
    httpd_resp_send_chunk(req, "<h2>PID Tuning Log</h2><table>", HTTPD_RESP_USE_STRLEN);

    char val[32];
    size_t bytes = pid_debug_log_get_size();
    snprintf(val, sizeof(val), "%u KB", (unsigned)((bytes + 512) / 1024));
    send_stat_row(req, "Current size", val);

    httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<p class='sub'>Records every heater PID cycle (target/measured temp, P/I/D terms, "
                           "fan and heater duty) in Profile and Manual mode while a real target temperature is "
                           "set - CSV, one row per follower tick (~1/s).</p>"
                           "<div class='btnrow'>"
                           "<a href='/api/pid_log/download'><button>Download CSV</button></a>"
                           "<button id='pidLogClearBtn' class='danger'>Clear Log</button>"
                           "</div>",
                           HTTPD_RESP_USE_STRLEN);
}

/* Operator-requested PID tuning aid: an open-loop step-response test -
 * commands the heater to a FIXED duty (bypassing the PID entirely) and logs
 * every tick (mode="STEPTEST" in the same CSV above) so the resulting
 * bean-temp rise curve can be used to characterize the plant's real thermal
 * lag/time constant for a proper retune, instead of guessing gains from
 * closed-loop data alone. Fan is set separately via the existing Fan level
 * buttons here (same /api/control action="set_fan" the dashboard uses) -
 * this hardware heats via forced air through the coil, so the fan can never
 * be off during a real heat test. */
static void send_step_test_section(httpd_req_t *req)
{
    int active_pct = profile_curve_follower_get_step_test_heater_pct();
    char buf[640];
    httpd_resp_send_chunk(req, "<h2>PID Tuning: Step-Response Test</h2>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<p class='sub'>Bypasses the PID: commands the heater to a fixed duty so you can record "
                           "how bean temp actually rises/settles. Set the fan level first (below), then Start. "
                           "Stop returns control to Manual/Profile mode and forces the heater off.</p>",
                           HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
             "<p><b>Status:</b> %s</p>"
             "<div class='btnrow'>"
             "<button data-fan='0'>Fan Off</button><button data-fan='1'>Fan L1 (80%%)</button>"
             "<button data-fan='2'>Fan L2 (90%%)</button><button data-fan='3'>Fan L3 (100%%)</button>"
             "</div>"
             "<div class='btnrow'>"
             "<input id='stepTestPct' type='number' min='0' max='100' value='%d' style='width:5em'> %%"
             "<button id='stepTestStartBtn'>Start</button>"
             "<button id='stepTestStopBtn' class='danger'>Stop</button>"
             "</div>",
             (active_pct >= 0) ? "RUNNING" : "stopped", (active_pct >= 0) ? active_pct : 50);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Operator-requested live PID tuning UI - see the /api/pid_tuning handlers.
 * Changing gains here takes effect on the very next control tick (and resets
 * the integral), so a tuning session doesn't need a rebuild/OTA per attempt. */
static void send_pid_tuning_section(httpd_req_t *req)
{
    heater_pid_tuning_t t;
    heater_pid_get_tuning(&t);

    char buf[2048];
    httpd_resp_send_chunk(req, "<h2>PID Tuning: Live Gains</h2>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<p class='sub'>Applies immediately to the running controller and resets the integral. "
                           "'Apply' is RAM-only (lost on reboot); 'Apply &amp; Save' also persists to NVS. "
                           "API: <code>GET/POST /api/pid_tuning</code> (form fields kp, ki, kd, margin_c, d_tau_s, "
                           "sp_ramp_c_s, duty_dz_pct, duty_gamma).</p>",
                           HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
             "<div class='btnrow'>"
             "<label>Kp <input id='pidKp' type='number' step='0.01' min='0' value='%.4f' style='width:6em'></label>"
             "<label>Ki <input id='pidKi' type='number' step='0.001' min='0' value='%.4f' style='width:6em'></label>"
             "<label>Kd <input id='pidKd' type='number' step='0.1' min='0' value='%.4f' style='width:6em'></label>"
             "<label>Cutoff margin &deg;C <input id='pidMargin' type='number' step='0.5' min='0.5' value='%.2f' style='width:6em'></label>"
             "<label>D filter &tau; s <input id='pidDTau' type='number' step='0.5' min='0' value='%.2f' style='width:6em'></label>"
             "<label>Setpoint ramp &deg;C/s <input id='pidSpRamp' type='number' step='0.1' min='0' value='%.2f' style='width:6em'></label>"
             "<label>Duty dead zone %% <input id='pidDutyDz' type='number' step='1' min='0' max='99' value='%.1f' style='width:6em'></label>"
             "<label>Duty curve &gamma; <input id='pidDutyGamma' type='number' step='0.1' min='0.1' value='%.2f' style='width:6em'></label>"
             "</div>"
             "<p class='sub'>D filter &tau;: low-pass filters the derivative term only, to reduce oscillation from "
             "sensor lag/noise amplified by Kd. 0 = no filtering (raw derivative). Setpoint ramp: caps how fast the "
             "internal target can move toward a big jump (Manual Target Temp, PREHEAT) instead of stepping to it "
             "instantly, so the integral doesn't wind up and overshoot on the first approach. 0 = no ramping "
             "(instant target). Duty dead zone/curve &gamma;: compensates the heater element's own nonlinear "
             "response (operator-reported: barely heats below a threshold duty, then gets much more aggressive "
             "near 100%%) by remapping the PID's logical output onto the physical SSR duty - dead zone 0%% and "
             "&gamma;=1.0 together disable this (physical=logical). Only affects Profile/Manual closed-loop "
             "control - Step Test and Autotune always command the exact raw duty requested.</p>"
             "<div class='btnrow'>"
             "<button id='pidTuneApplyBtn'>Apply</button>"
             "<button id='pidTuneSaveBtn'>Apply &amp; Save</button>"
             "<span id='pidTuneStatus' class='sub'></span>"
             "</div>",
             (double)t.kp, (double)t.ki, (double)t.kd, (double)t.hard_overshoot_margin_c, (double)t.d_filter_tau_s,
             (double)t.setpoint_ramp_c_per_s, (double)t.duty_curve_deadzone_pct, (double)t.duty_curve_gamma);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Relay-feedback autotune UI - see the /api/pid_autotune handlers. The
 * setpoint deliberately defaults well below the tunnel thermostat's measured
 * ~194 C trip point, and the relay's high output is capped, because this
 * procedure works by making the temperature oscillate on purpose. */
static void send_pid_autotune_section(httpd_req_t *req)
{
    pid_autotune_status_t st;
    pid_autotune_get_status(&st);

    const char *state_str = "idle";
    switch (st.state) {
    case PID_AUTOTUNE_RUNNING:   state_str = "RUNNING"; break;
    case PID_AUTOTUNE_SUCCEEDED: state_str = "SUCCEEDED"; break;
    case PID_AUTOTUNE_FAILED:    state_str = "FAILED"; break;
    default: break;
    }

    char buf[1024];
    httpd_resp_send_chunk(req, "<h2>PID Tuning: Autotune (relay method)</h2>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                           "<p class='sub'>Switches the heater between two fixed duties to make bean temp oscillate "
                           "around the setpoint, then derives Ku/Pu and Ziegler-Nichols gains. "
                           "Set the fan level FIRST - the resulting gains are only valid for that airflow. "
                           "If Max Heater Power (above) is capped below 100%, the relay's requested output is "
                           "scaled down before it reaches the heater - the result is still correct but the run "
                           "will be slower; consider setting it to 100% first. Aborts by itself on overshoot, "
                           "fan drop or sensor fault.</p>",
                           HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
             "<p><b>Status:</b> %s &mdash; %s<br>"
             "<span class='sub'>elapsed %us, phase %u/%u, crossings %u, fan at start %u%%, heater cap %u%% "
             "(effective high %u%%)</span></p>"
             "<div class='btnrow'>"
             "<label>Setpoint &deg;C <input id='atSetpoint' type='number' step='1' min='40' max='200' value='%.0f' style='width:6em'></label>"
             "<label>Relay high %% <input id='atHigh' type='number' step='1' min='1' max='100' value='%u' style='width:5em'></label>"
             "<label>Relay low %% <input id='atLow' type='number' step='1' min='0' max='99' value='%u' style='width:5em'></label>"
             "</div>"
             "<div class='btnrow'>"
             "<button id='atStartBtn'>Start Autotune</button>"
             "<button id='atStopBtn' class='danger'>Abort</button>"
             "<span id='atStatus' class='sub'></span>"
             "</div>"
             "<div id='atResult' class='sub'></div>",
             state_str, st.message, (unsigned)st.elapsed_s, (unsigned)st.phase_count, (unsigned)st.phase_count_max,
             (unsigned)st.zc_count, (unsigned)st.fan_pct_at_start, (unsigned)st.heater_power_cap_pct,
             (unsigned)st.effective_output_positive_pct,
             (double)((st.setpoint_c > 0.0f) ? st.setpoint_c : 150.0f),
             (unsigned)((st.output_positive_pct > 0) ? st.output_positive_pct : 70),
             (unsigned)st.output_negative_pct);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t diagnostics_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    web_ui_enable_low_latency(req);
    httpd_resp_send_chunk(req,
                           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                           "<title>Pop Roaster - Diagnostics</title>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WEB_UI_STYLE_LINK, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</head><body><div class='app'>", HTTPD_RESP_USE_STRLEN);
    web_ui_send_nav_bar(req, "diagnostics");
    httpd_resp_send_chunk(req,
                           "<main class='content'><div class='card'>"
                           "<div style='display:flex;justify-content:space-between;align-items:center'>"
                           "<h1>Diagnostics</h1><button onclick='location.reload()'>Refresh</button></div>"
                           "<p class='sub'>Read-only - for troubleshooting/monitoring only. Tap Refresh for current values.</p>",
                           HTTPD_RESP_USE_STRLEN);

    send_system_section(req);
    send_pid_log_section(req);
    send_pid_tuning_section(req);
    send_pid_autotune_section(req);
    send_step_test_section(req);
    send_heap_section(req, "Internal RAM (heap)", MALLOC_CAP_INTERNAL);
    send_heap_section(req, "PSRAM (external RAM)", MALLOC_CAP_SPIRAM);
    send_nvs_section(req);
    send_tasks_section(req);

    httpd_resp_send_chunk(req,
                           "<script>"
                           "document.getElementById('pidLogClearBtn').addEventListener('click',function(){"
                           "if(!confirm('Clear the PID tuning log?'))return;"
                           "fetch('/api/pid_log/clear',{method:'POST'}).then(function(){location.reload();});"
                           "});"
                           "function postControl(action,value){"
                           "return fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
                           "body:'action='+action+'&value='+value});"
                           "}"
                           "document.querySelectorAll('[data-fan]').forEach(function(btn){"
                           "btn.addEventListener('click',function(){postControl('set_fan',btn.getAttribute('data-fan'));});"
                           "});"
                           "document.getElementById('stepTestStartBtn').addEventListener('click',function(){"
                           "var pct=document.getElementById('stepTestPct').value;"
                           "postControl('set_step_test_heater',pct).then(function(){location.reload();});"
                           "});"
                           "document.getElementById('stepTestStopBtn').addEventListener('click',function(){"
                           "postControl('set_step_test_heater',-1).then(function(){location.reload();});"
                           "});"
                           "function applyTuning(persist){"
                           "var b='kp='+document.getElementById('pidKp').value"
                           "+'&ki='+document.getElementById('pidKi').value"
                           "+'&kd='+document.getElementById('pidKd').value"
                           "+'&margin_c='+document.getElementById('pidMargin').value"
                           "+'&d_tau_s='+document.getElementById('pidDTau').value"
                           "+'&sp_ramp_c_s='+document.getElementById('pidSpRamp').value"
                           "+'&duty_dz_pct='+document.getElementById('pidDutyDz').value"
                           "+'&duty_gamma='+document.getElementById('pidDutyGamma').value"
                           "+(persist?'&persist=1':'');"
                           "fetch('/api/pid_tuning',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
                           ".then(function(r){return r.json();})"
                           ".then(function(j){document.getElementById('pidTuneStatus').textContent="
                           "'Applied: Kp='+j.kp+' Ki='+j.ki+' Kd='+j.kd+' margin='+j.margin_c+' d_tau='+j.d_tau_s"
                           "+' sp_ramp='+j.sp_ramp_c_s+' duty_dz='+j.duty_dz_pct+' duty_gamma='+j.duty_gamma+(persist?' (saved)':'');})"
                           ".catch(function(){document.getElementById('pidTuneStatus').textContent='Failed';});"
                           "}"
                           "document.getElementById('pidTuneApplyBtn').addEventListener('click',function(){applyTuning(false);});"
                           "document.getElementById('pidTuneSaveBtn').addEventListener('click',function(){applyTuning(true);});"
                           "var atLast=null;"
                           "function atRow(key,n,g){"
                           "return '<div>'+n+': Kp='+g.kp.toFixed(3)+' Ki='+g.ki.toFixed(4)+' Kd='+g.kd.toFixed(2)"
                           "+' <button data-gains='+key+'>Use</button></div>';"
                           "}"
                           "function atUse(key){"
                           "if(!atLast)return;"
                           "var g=atLast[key];"
                           "document.getElementById('pidKp').value=g.kp.toFixed(4);"
                           "document.getElementById('pidKi').value=g.ki.toFixed(4);"
                           "document.getElementById('pidKd').value=g.kd.toFixed(4);"
                           "document.getElementById('pidKp').scrollIntoView();"
                           "}"
                           "document.getElementById('atResult').addEventListener('click',function(e){"
                           "var k=e.target.getAttribute('data-gains');if(k)atUse(k);"
                           "});"
                           "function atPoll(){"
                           "fetch('/api/pid_autotune').then(function(r){return r.json();}).then(function(j){"
                           "atLast=j;"
                           "document.getElementById('atStatus').textContent=j.state+' - '+j.message"
                           "+' ('+j.elapsed_s+'s, phase '+j.phases+'/'+j.max_phases+')';"
                           "if(j.state==='SUCCEEDED'){"
                           "document.getElementById('atResult').innerHTML='<b>Ku='+j.ku.toFixed(2)+' Pu='+j.pu_s.toFixed(1)+'s</b>'"
                           "+atRow('zn_classic','Ziegler-Nichols',j.zn_classic)"
                           "+atRow('some_overshoot','Some overshoot',j.some_overshoot)"
                           "+atRow('no_overshoot','No overshoot',j.no_overshoot)"
                           "+(j.zc_symmetrical?'':'<div>Warning: heating/cooling rates asymmetric - lower the relay high %</div>')"
                           "+(j.amplitude_convergent?'':'<div>Warning: amplitude did not converge - result is unreliable</div>');"
                           "}"
                           "if(j.state==='RUNNING')setTimeout(atPoll,3000);"
                           "});"
                           "}"
                           "document.getElementById('atStartBtn').addEventListener('click',function(){"
                           "var b='setpoint_c='+document.getElementById('atSetpoint').value"
                           "+'&output_positive_pct='+document.getElementById('atHigh').value"
                           "+'&output_negative_pct='+document.getElementById('atLow').value;"
                           "fetch('/api/pid_autotune',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
                           ".then(function(){atPoll();});"
                           "});"
                           "document.getElementById('atStopBtn').addEventListener('click',function(){"
                           "fetch('/api/pid_autotune',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'abort=1'})"
                           ".then(function(){atPoll();});"
                           "});"
                           "atPoll();"
                           "</script>",
                           HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Diagnostics page shown");
    return ESP_OK;
}

/* Streams the raw PID debug log CSV file for download - same chunked-file
 * pattern as history_routes.c's session export. */
static esp_err_t pid_log_download_get_handler(httpd_req_t *req)
{
    FILE *f = fopen("/storage/pid_debug.csv", "r");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No PID log yet");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pid_debug.csv\"");

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pid_log_clear_post_handler(httpd_req_t *req)
{
    esp_err_t err = pid_debug_log_clear();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, (err == ESP_OK) ? "OK" : esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Operator-requested live PID tuning API ("pode criar uma API para poder
 * acessar localmente para capturar o log e alterar os parametros em tempo
 * real"): lets gains be read and changed while the roaster is running, so a
 * tuning session iterates in seconds instead of one rebuild+OTA per attempt.
 *
 *   GET  /api/pid_tuning  -> {"kp":1.5,"ki":0.034,"kd":5.3,"margin_c":8.0,"d_tau_s":2.0,"sp_ramp_c_s":1.0,
 *                             "duty_dz_pct":65.0,"duty_gamma":2.0}
 *   POST /api/pid_tuning  -> form-encoded kp/ki/kd/margin_c/d_tau_s/sp_ramp_c_s/duty_dz_pct/duty_gamma
 *                            (any subset; omitted fields keep their current
 *                            value), plus optional persist=1 to save to NVS.
 */
static esp_err_t pid_tuning_get_handler(httpd_req_t *req)
{
    heater_pid_tuning_t t;
    heater_pid_get_tuning(&t);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.4f,\"margin_c\":%.2f,\"d_tau_s\":%.2f,\"sp_ramp_c_s\":%.2f,"
             "\"duty_dz_pct\":%.1f,\"duty_gamma\":%.2f}",
             (double)t.kp, (double)t.ki, (double)t.kd, (double)t.hard_overshoot_margin_c, (double)t.d_filter_tau_s,
             (double)t.setpoint_ramp_c_per_s, (double)t.duty_curve_deadzone_pct, (double)t.duty_curve_gamma);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/** Reads one float field from a form-encoded body, or leaves `*out` as-is (negative sentinel) when absent. */
static void parse_float_field(const char *body, const char *key, float *out)
{
    char param[32];
    if (httpd_query_key_value(body, key, param, sizeof(param)) == ESP_OK) {
        *out = strtof(param, NULL);
    }
}

static esp_err_t pid_tuning_post_handler(httpd_req_t *req)
{
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Negative sentinels mean "field not supplied" - heater_pid_set_tuning()
     * leaves those gains untouched. */
    heater_pid_tuning_t t = { .kp = -1.0f, .ki = -1.0f, .kd = -1.0f, .hard_overshoot_margin_c = -1.0f,
                              .d_filter_tau_s = -1.0f, .setpoint_ramp_c_per_s = -1.0f,
                              .duty_curve_deadzone_pct = -1.0f, .duty_curve_gamma = -1.0f };
    parse_float_field(body, "kp", &t.kp);
    parse_float_field(body, "ki", &t.ki);
    parse_float_field(body, "kd", &t.kd);
    parse_float_field(body, "margin_c", &t.hard_overshoot_margin_c);
    parse_float_field(body, "d_tau_s", &t.d_filter_tau_s);
    parse_float_field(body, "sp_ramp_c_s", &t.setpoint_ramp_c_per_s);
    parse_float_field(body, "duty_dz_pct", &t.duty_curve_deadzone_pct);
    parse_float_field(body, "duty_gamma", &t.duty_curve_gamma);

    char persist_param[8] = { 0 };
    bool persist = (httpd_query_key_value(body, "persist", persist_param, sizeof(persist_param)) == ESP_OK) &&
                   (strcmp(persist_param, "1") == 0);

    esp_err_t err = heater_pid_set_tuning(&t, persist);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return pid_tuning_get_handler(req); /* Echo back the values now in effect. */
}

/*   GET  /api/pid_autotune -> full status + the three candidate rule sets
 *   POST /api/pid_autotune -> form-encoded setpoint_c/output_positive_pct/
 *                             output_negative_pct to start, or abort=1 to stop.
 */
static esp_err_t pid_autotune_get_handler(httpd_req_t *req)
{
    pid_autotune_status_t st;
    pid_autotune_get_status(&st);

    const char *state_str = "IDLE";
    switch (st.state) {
    case PID_AUTOTUNE_RUNNING:   state_str = "RUNNING"; break;
    case PID_AUTOTUNE_SUCCEEDED: state_str = "SUCCEEDED"; break;
    case PID_AUTOTUNE_FAILED:    state_str = "FAILED"; break;
    default: break;
    }

    char body[768];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"message\":\"%s\",\"setpoint_c\":%.1f,\"fan_pct\":%u,\"elapsed_s\":%u,"
             "\"phases\":%u,\"max_phases\":%u,\"crossings\":%u,\"ku\":%.4f,\"pu_s\":%.2f,"
             "\"heater_cap_pct\":%u,\"effective_positive_pct\":%u,\"relay_positive\":%s,"
             "\"zc_symmetrical\":%s,\"amplitude_convergent\":%s,"
             "\"zn_classic\":{\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.4f},"
             "\"some_overshoot\":{\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.4f},"
             "\"no_overshoot\":{\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.4f}}",
             state_str, st.message, (double)st.setpoint_c, (unsigned)st.fan_pct_at_start,
             (unsigned)st.elapsed_s, (unsigned)st.phase_count, (unsigned)st.phase_count_max,
             (unsigned)st.zc_count, (double)st.ku, (double)st.pu_s, (unsigned)st.heater_power_cap_pct,
             (unsigned)st.effective_output_positive_pct, st.relay_positive ? "true" : "false",
             st.zc_symmetrical ? "true" : "false",
             st.amplitude_convergent ? "true" : "false", (double)st.zn_classic.kp, (double)st.zn_classic.ki,
             (double)st.zn_classic.kd, (double)st.some_overshoot.kp, (double)st.some_overshoot.ki,
             (double)st.some_overshoot.kd, (double)st.no_overshoot.kp, (double)st.no_overshoot.ki,
             (double)st.no_overshoot.kd);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t pid_autotune_post_handler(httpd_req_t *req)
{
    char body[192];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char param[16];
    if (httpd_query_key_value(body, "abort", param, sizeof(param)) == ESP_OK) {
        pid_autotune_abort("Aborted by operator");
        return pid_autotune_get_handler(req);
    }

    float setpoint_c = -1.0f;
    parse_float_field(body, "setpoint_c", &setpoint_c);
    long high = 70;
    long low = 0;
    if (httpd_query_key_value(body, "output_positive_pct", param, sizeof(param)) == ESP_OK) {
        high = strtol(param, NULL, 10);
    }
    if (httpd_query_key_value(body, "output_negative_pct", param, sizeof(param)) == ESP_OK) {
        low = strtol(param, NULL, 10);
    }
    if (setpoint_c <= 0.0f || high < 1 || high > 100 || low < 0 || low >= high) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad setpoint or relay outputs");
        return ESP_FAIL;
    }

    esp_err_t err = pid_autotune_start(setpoint_c, (uint8_t)high, (uint8_t)low);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return pid_autotune_get_handler(req);
}

esp_err_t diagnostics_routes_register(httpd_handle_t server)
{
    httpd_uri_t uri = { .uri = "/diagnostics", .method = HTTP_GET, .handler = diagnostics_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t download_uri = { .uri = "/api/pid_log/download", .method = HTTP_GET, .handler = pid_log_download_get_handler };
    err = httpd_register_uri_handler(server, &download_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t clear_uri = { .uri = "/api/pid_log/clear", .method = HTTP_POST, .handler = pid_log_clear_post_handler };
    err = httpd_register_uri_handler(server, &clear_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t tuning_get_uri = { .uri = "/api/pid_tuning", .method = HTTP_GET, .handler = pid_tuning_get_handler };
    err = httpd_register_uri_handler(server, &tuning_get_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t tuning_post_uri = { .uri = "/api/pid_tuning", .method = HTTP_POST, .handler = pid_tuning_post_handler };
    err = httpd_register_uri_handler(server, &tuning_post_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t autotune_get_uri = { .uri = "/api/pid_autotune", .method = HTTP_GET, .handler = pid_autotune_get_handler };
    err = httpd_register_uri_handler(server, &autotune_get_uri);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t autotune_post_uri = { .uri = "/api/pid_autotune", .method = HTTP_POST, .handler = pid_autotune_post_handler };
    err = httpd_register_uri_handler(server, &autotune_post_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Diagnostics routes registered (/diagnostics, /api/pid_log/*, /api/pid_tuning, /api/pid_autotune)");
    return ESP_OK;
}

/**
 * @file diagnostics_routes.c
 * @brief See header.
 */
#include <stdio.h>
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
    send_heap_section(req, "Internal RAM (heap)", MALLOC_CAP_INTERNAL);
    send_heap_section(req, "PSRAM (external RAM)", MALLOC_CAP_SPIRAM);
    send_nvs_section(req);
    send_tasks_section(req);

    httpd_resp_send_chunk(req, "</div></main></div></body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Diagnostics page shown");
    return ESP_OK;
}

esp_err_t diagnostics_routes_register(httpd_handle_t server)
{
    httpd_uri_t uri = { .uri = "/diagnostics", .method = HTTP_GET, .handler = diagnostics_get_handler };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Diagnostics routes registered (/diagnostics)");
    return ESP_OK;
}

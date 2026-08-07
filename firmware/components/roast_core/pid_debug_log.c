/**
 * @file pid_debug_log.c
 * @brief See header.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "roast_core/heater_pid.h"
#include "roast_core/pid_debug_log.h"

static const char *TAG = "pid_debug_log";

#define PID_DEBUG_LOG_PATH "/storage/pid_debug.csv"
/* Bounds storage usage on this small (896KB, shared with roast history)
 * LittleFS partition - truncates/restarts once exceeded rather than
 * growing forever. ~300KB is roughly 40-50 minutes of continuous 1Hz
 * logging, comfortably enough for one tuning test run. */
#define PID_DEBUG_LOG_MAX_BYTES (300 * 1024)

/* WEB-RESPONSIVENESS FIX (operator-reported: "a pagina Web nao abria, tem
 * hora que abre rapido, tem hora que para de responder" while a PID test was
 * running). pid_debug_log_record() is called from profile_curve_follower.c's
 * 1Hz esp_timer callback, which runs in the esp_timer task at priority 22 -
 * far ABOVE the HTTP server task (priority 5). The original implementation
 * did all its flash I/O inline there: a full fopen/fseek/ftell/fclose just to
 * check the size cap, then another fopen(append)/fprintf/fclose to write the
 * row - two complete LittleFS open/close cycles, every single second. Each
 * LittleFS open-for-append has to walk the file's CTZ skip-list and each
 * close syncs metadata blocks, so the cost grows as the log file grows, and
 * the whole time it holds the filesystem lock that the web handlers (page
 * render, history, OTA, log download) also need. The high-priority timer task
 * therefore periodically starved the HTTP task mid-request - exactly the
 * intermittent "sometimes instant, sometimes hangs" behavior reported, and it
 * got worse the longer a test ran.
 *
 * Now: record() only formats the row into a small stack buffer and drops it
 * on a queue (non-blocking, never waits, discards if full), and a dedicated
 * LOW-priority writer task does the actual flash I/O, batching everything
 * currently queued into a single open/close. The file size is tracked in RAM
 * so the cap check costs nothing. Net effect: the 1Hz control path no longer
 * touches the filesystem at all, and the web server can always preempt the
 * writer. */
#define PID_LOG_LINE_MAX 224
#define PID_LOG_QUEUE_LEN 16
#define PID_LOG_TASK_PRIO 3 /* Deliberately below the HTTP server task (5) so web requests always win. */
#define PID_LOG_TASK_STACK 3072

typedef struct {
    char line[PID_LOG_LINE_MAX];
} pid_log_entry_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_file_mutex;
static size_t s_cached_size;

/* kp/ki/kd are logged on EVERY row (not just once) specifically so a live
 * tuning session via POST /api/pid_tuning is analyzable offline: the gains
 * can change mid-file, and without them per-row there's no way to tell which
 * rows belong to which attempt. */
static const char *CSV_HEADER =
    "t_ms,mode,phase,elapsed_s,target_c,measured_c,sensor_valid,error_c,p_term,i_term,d_term,"
    "pid_raw_pct,hard_cutoff,requested_heater_pct,applied_heater_pct,target_fan_pct,real_fan_pct,"
    "kp,ki,kd\n";

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

/** Reads the on-disk size once (used only at init and after a clear - the running size is tracked in RAM). */
static size_t read_file_size(void)
{
    FILE *f = fopen(PID_DEBUG_LOG_PATH, "r");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return (size > 0) ? (size_t)size : 0;
}

/** Low-priority writer: the ONLY place that touches the log file during normal operation. */
static void pid_log_writer_task(void *arg)
{
    (void)arg;
    pid_log_entry_t entry;
    for (;;) {
        if (xQueueReceive(s_queue, &entry, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_file_mutex, portMAX_DELAY);

        if (s_cached_size >= PID_DEBUG_LOG_MAX_BYTES) {
            /* Cap reached - restart the file rather than growing forever. */
            FILE *reset_f = fopen(PID_DEBUG_LOG_PATH, "w");
            if (reset_f != NULL) {
                fputs(CSV_HEADER, reset_f);
                fclose(reset_f);
                s_cached_size = strlen(CSV_HEADER);
            }
        }

        FILE *f = fopen(PID_DEBUG_LOG_PATH, "a");
        if (f != NULL) {
            /* Batch: write the row we just took plus anything else that
             * queued up meanwhile, so a burst costs one open/close instead
             * of one per row. */
            do {
                size_t len = strlen(entry.line);
                if (fwrite(entry.line, 1, len, f) == len) {
                    s_cached_size += len;
                }
            } while (xQueueReceive(s_queue, &entry, 0) == pdTRUE);
            fclose(f);
        }

        xSemaphoreGive(s_file_mutex);
    }
}

esp_err_t pid_debug_log_init(void)
{
    /* Storage partition is already mounted by storage/session_store.c's
     * session_store_init() (called earlier in app_main()) - nothing to do
     * here except make sure the CSV header exists if the file is new. */
    FILE *f = fopen(PID_DEBUG_LOG_PATH, "r");
    if (f != NULL) {
        fclose(f);
    } else {
        f = fopen(PID_DEBUG_LOG_PATH, "w");
        if (f == NULL) {
            ESP_LOGW(TAG, "Failed to create %s", PID_DEBUG_LOG_PATH);
            return ESP_FAIL;
        }
        fputs(CSV_HEADER, f);
        fclose(f);
    }

    s_cached_size = read_file_size();

    s_file_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(PID_LOG_QUEUE_LEN, sizeof(pid_log_entry_t));
    if (s_file_mutex == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate log queue/mutex");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(pid_log_writer_task, "pid_log_wr", PID_LOG_TASK_STACK, NULL, PID_LOG_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create log writer task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PID debug log ready at %s (%u bytes)", PID_DEBUG_LOG_PATH, (unsigned)s_cached_size);
    return ESP_OK;
}

void pid_debug_log_record(const char *mode_str, roast_phase_t phase, uint32_t elapsed_s, float target_temp_c,
                           float measured_temp_c, bool sensor_valid, uint8_t requested_heater_pct,
                           uint8_t applied_heater_pct, uint8_t target_fan_pct, uint8_t real_fan_pct)
{
    if (s_queue == NULL) {
        return; /* Not initialized - best-effort, never affect the control loop. */
    }

    heater_pid_debug_t dbg;
    heater_pid_get_last_debug(&dbg);

    heater_pid_tuning_t tuning;
    heater_pid_get_tuning(&tuning);

    pid_log_entry_t entry;
    snprintf(entry.line, sizeof(entry.line),
             "%lld,%s,%s,%u,%.1f,%.1f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%u,%u,%u,%u,%.4f,%.4f,%.4f\n",
             (long long)(esp_timer_get_time() / 1000), mode_str, phase_str(phase), (unsigned)elapsed_s,
             (double)target_temp_c, (double)measured_temp_c, (int)sensor_valid, (double)dbg.error_c,
             (double)dbg.p_term, (double)dbg.i_term, (double)dbg.d_term, (double)dbg.raw_output,
             (int)dbg.hard_cutoff, (unsigned)requested_heater_pct, (unsigned)applied_heater_pct,
             (unsigned)target_fan_pct, (unsigned)real_fan_pct,
             (double)tuning.kp, (double)tuning.ki, (double)tuning.kd);

    /* Zero timeout: this runs on the high-priority esp_timer task driving the
     * 1Hz control loop - it must never block on storage. Dropping a row under
     * extreme load is strictly better than delaying heater/fan commands. */
    (void)xQueueSend(s_queue, &entry, 0);
}

esp_err_t pid_debug_log_clear(void)
{
    if (s_file_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    FILE *f = fopen(PID_DEBUG_LOG_PATH, "w");
    if (f == NULL) {
        xSemaphoreGive(s_file_mutex);
        return ESP_FAIL;
    }
    fputs(CSV_HEADER, f);
    fclose(f);
    s_cached_size = strlen(CSV_HEADER);
    xSemaphoreGive(s_file_mutex);
    ESP_LOGI(TAG, "PID debug log cleared");
    return ESP_OK;
}

size_t pid_debug_log_get_size(void)
{
    /* Served from RAM so the Diagnostics page render never hits the filesystem. */
    return s_cached_size;
}

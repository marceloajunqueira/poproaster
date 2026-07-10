/**
 * @file duration_watchdog.c
 * @brief See header.
 */
#include "esp_log.h"
#include "esp_timer.h"

#include "roast_core/session_state_machine.h"
#include "safety/duration_watchdog.h"
#include "safety/safety_manager.h"

static const char *TAG = "duration_watchdog";

#define CHECK_PERIOD_US (1000 * 1000)

static int64_t s_max_duration_ms = SAFETY_DEFAULT_MAX_DURATION_MS;
static esp_timer_handle_t s_timer;

static void check_timer_cb(void *arg)
{
    (void)arg;

    const roast_session_t *session = session_sm_get_state();
    bool actively_heating = (session->phase == ROAST_PHASE_ROASTING || session->phase == ROAST_PHASE_DEVELOPMENT);
    if (!actively_heating) {
        return;
    }

    if (session->elapsed_ms >= s_max_duration_ms) {
        ESP_LOGW(TAG, "Max roast duration exceeded (%lld ms >= %lld ms) - forcing auto-cooling",
                 (long long)session->elapsed_ms, (long long)s_max_duration_ms);
        /* FR-033: raise the critical alarm (requires manual ack, FR-029)
         * AND force the automatic transition to Cooling (heater off, fan
         * stays on, per T025) - both happen regardless of operator action. */
        safety_manager_report_duration_exceeded();
        session_sm_start_cooling();
    }
}

esp_err_t duration_watchdog_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = check_timer_cb,
        .name = "duration_watchdog",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, CHECK_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Duration watchdog started (max=%lld ms)", (long long)s_max_duration_ms);
    return ESP_OK;
}

void duration_watchdog_set_max_duration_ms(int64_t max_duration_ms)
{
    if (max_duration_ms > 0) {
        s_max_duration_ms = max_duration_ms;
    }
}

int64_t duration_watchdog_get_max_duration_ms(void)
{
    return s_max_duration_ms;
}

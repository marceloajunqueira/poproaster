/**
 * @file roast_events.c
 * @brief See header.
 */
#include <string.h>
#include "esp_log.h"

#include "roast_core/session_state_machine.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/roast_events.h"

static const char *TAG = "roast_events";

static roast_event_record_t s_events[ROAST_EVENTS_MAX];
static size_t s_event_count = 0;

void roast_events_reset(void)
{
    s_event_count = 0;
}

esp_err_t roast_events_mark(roast_event_type_t type)
{
    const roast_session_t *session = session_sm_get_state();
    return roast_events_mark_at(type, session->elapsed_ms);
}

esp_err_t roast_events_mark_at(roast_event_type_t type, int64_t elapsed_ms)
{
    if (type < 0 || type >= ROAST_EVENT_TYPE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_event_count < ROAST_EVENTS_MAX) {
        s_events[s_event_count].type = type;
        s_events[s_event_count].elapsed_ms = elapsed_ms;
        s_event_count++;
    } else {
        ESP_LOGW(TAG, "Event list full (%d) - marker not kept for live chart, still recorded to history", ROAST_EVENTS_MAX);
    }

    roast_telemetry_service_record_event(elapsed_ms, (int)type);

    if (type == ROAST_EVENT_FC_START) {
        roast_telemetry_service_mark_first_crack();
    }

    ESP_LOGI(TAG, "Marked event %s at %lldms", roast_event_full_label(type), (long long)elapsed_ms);
    return ESP_OK;
}

size_t roast_events_get_all(roast_event_record_t *out, size_t max)
{
    if (out == NULL) {
        return 0;
    }
    size_t n = (s_event_count < max) ? s_event_count : max;
    memcpy(out, s_events, n * sizeof(roast_event_record_t));
    return n;
}

size_t roast_events_get_count(void)
{
    return s_event_count;
}

const char *roast_event_short_label(roast_event_type_t type)
{
    switch (type) {
    case ROAST_EVENT_TURNING_POINT: return "TP";
    case ROAST_EVENT_FC_START: return "FCs";
    case ROAST_EVENT_FC_END: return "FCe";
    case ROAST_EVENT_SC_START: return "SCs";
    case ROAST_EVENT_SC_END: return "SCe";
    case ROAST_EVENT_COOL_START: return "Cool";
    default: return "?";
    }
}

const char *roast_event_full_label(roast_event_type_t type)
{
    switch (type) {
    case ROAST_EVENT_TURNING_POINT: return "Turning Point";
    case ROAST_EVENT_FC_START: return "First Crack Start";
    case ROAST_EVENT_FC_END: return "First Crack End";
    case ROAST_EVENT_SC_START: return "Second Crack Start";
    case ROAST_EVENT_SC_END: return "Second Crack End";
    case ROAST_EVENT_COOL_START: return "Cool Start";
    default: return "Unknown";
    }
}

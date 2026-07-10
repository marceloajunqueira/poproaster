/**
 * @file ror_calculator.c
 * @brief RoR calculator implementation using a small ring buffer of samples.
 */
#include <stdlib.h>
#include <string.h>

#include "roast_core/ror_calculator.h"

#define ROR_MAX_SAMPLES 64

typedef struct {
    float temp_c;
    int64_t timestamp_ms;
} ror_sample_t;

struct ror_calculator_s {
    ror_sample_t samples[ROR_MAX_SAMPLES];
    int count;
    int head;
    uint32_t smoothing_window_ms;
    float last_ror_c_per_min;
};

ror_calculator_handle_t ror_calculator_create(uint32_t smoothing_window_ms)
{
    struct ror_calculator_s *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    h->smoothing_window_ms = smoothing_window_ms > 0 ? smoothing_window_ms : 30000;
    return h;
}

void ror_calculator_destroy(ror_calculator_handle_t handle)
{
    free(handle);
}

bool ror_calculator_add_sample(ror_calculator_handle_t handle, float temp_c, int64_t timestamp_ms)
{
    if (handle == NULL) {
        return false;
    }

    handle->samples[handle->head] = (ror_sample_t){.temp_c = temp_c, .timestamp_ms = timestamp_ms};
    handle->head = (handle->head + 1) % ROR_MAX_SAMPLES;
    if (handle->count < ROR_MAX_SAMPLES) {
        handle->count++;
    }

    /* Find the oldest sample still within the smoothing window. */
    ror_sample_t *oldest_in_window = NULL;
    for (int i = 0; i < handle->count; i++) {
        int idx = (handle->head - 1 - i + ROR_MAX_SAMPLES) % ROR_MAX_SAMPLES;
        ror_sample_t *s = &handle->samples[idx];
        if ((timestamp_ms - s->timestamp_ms) <= (int64_t)handle->smoothing_window_ms) {
            oldest_in_window = s;
        } else {
            break;
        }
    }

    if (oldest_in_window == NULL || oldest_in_window->timestamp_ms == timestamp_ms) {
        return false; /* Not enough history yet. */
    }

    float dt_min = (float)(timestamp_ms - oldest_in_window->timestamp_ms) / 60000.0f;
    if (dt_min <= 0.0f) {
        return false;
    }
    float dtemp = temp_c - oldest_in_window->temp_c;
    handle->last_ror_c_per_min = dtemp / dt_min;
    return true;
}

float ror_calculator_get_ror_c_per_min(ror_calculator_handle_t handle)
{
    return handle ? handle->last_ror_c_per_min : 0.0f;
}

void ror_calculator_reset(ror_calculator_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    handle->count = 0;
    handle->head = 0;
    handle->last_ror_c_per_min = 0.0f;
}

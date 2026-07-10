/**
 * @file dtr_calculator.h
 * @brief Development Time Ratio (DTR%) calculator.
 *
 * FR-037: DTR% = (time since FIRST_CRACK_START / total roast time so far) * 100,
 * computed in real time once the FIRST_CRACK_START event has been marked.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool first_crack_marked;
    int64_t first_crack_at_ms;
} dtr_calculator_state_t;

void dtr_calculator_reset(dtr_calculator_state_t *state);

/** Marks the FIRST_CRACK_START event at the given elapsed-roast-time (ms). */
void dtr_calculator_mark_first_crack(dtr_calculator_state_t *state, int64_t elapsed_roast_ms);

/**
 * Returns the current DTR% given the total elapsed roast time so far (ms).
 * Returns -1.0f if first crack has not been marked yet (no valid DTR%).
 */
float dtr_calculator_get_pct(const dtr_calculator_state_t *state, int64_t elapsed_roast_ms);

/**
 * @file dtr_calculator.c
 * @brief DTR% calculator implementation.
 */
#include "roast_core/dtr_calculator.h"

void dtr_calculator_reset(dtr_calculator_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->first_crack_marked = false;
    state->first_crack_at_ms = 0;
}

void dtr_calculator_mark_first_crack(dtr_calculator_state_t *state, int64_t elapsed_roast_ms)
{
    if (state == NULL) {
        return;
    }
    state->first_crack_marked = true;
    state->first_crack_at_ms = elapsed_roast_ms;
}

float dtr_calculator_get_pct(const dtr_calculator_state_t *state, int64_t elapsed_roast_ms)
{
    if (state == NULL || !state->first_crack_marked || elapsed_roast_ms <= 0) {
        return -1.0f;
    }
    int64_t development_ms = elapsed_roast_ms - state->first_crack_at_ms;
    if (development_ms < 0) {
        development_ms = 0;
    }
    return ((float)development_ms / (float)elapsed_roast_ms) * 100.0f;
}

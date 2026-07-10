/**
 * @file duration_watchdog.h
 * @brief FR-033: max roast duration watchdog. Forces an automatic Cooling
 *        transition + critical alarm if a roast stays in an active heating
 *        phase (ROASTING/DEVELOPMENT) longer than the configured limit
 *        (default 25 minutes), as a safety net against a forgotten/stuck
 *        roast.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the background watchdog timer. Call once at boot, after
 * session_state_machine_init() and safety_manager_init(). */
esp_err_t duration_watchdog_init(void);

/** Reconfigures the max duration (ms) - for a future Config screen
 * (T067)/NVS-backed setting; defaults to SAFETY_DEFAULT_MAX_DURATION_MS
 * (safety/safety_manager.h, 25 minutes) until then. */
void duration_watchdog_set_max_duration_ms(int64_t max_duration_ms);

/** Returns the currently configured max duration (ms). */
int64_t duration_watchdog_get_max_duration_ms(void);

#ifdef __cplusplus
}
#endif

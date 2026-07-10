/**
 * @file roast_telemetry_service.h
 * @brief Always-on background sampling of BT/RoR/DTR/fan/heater telemetry.
 *
 * Historically this sampling loop lived inside the roast dashboard screen's
 * own LVGL timer, which meant switching away from the dashboard tab (e.g. to
 * the History tab in the new nav_shell UI) silently paused RoR feeding and
 * session history recording. This service decouples sampling/recording from
 * whichever screen happens to be visible: it runs on its own esp_timer
 * (no LVGL dependency, no LVGL lock needed) starting at boot, and caches the
 * latest values in a small snapshot struct that any UI screen can poll
 * cheaply (no sensor I/O, no file I/O) whenever it needs to repaint labels
 * or a chart.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "roast_core/session_state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool sensor_valid;
    float bean_temp_c;
    float ror_c_per_min;
    float dtr_pct;       /* -1.0f if not available yet (first crack not marked). */
    int fan_pct;
    int heater_pct;
    roast_phase_t phase;
    bool paused;
    int64_t elapsed_ms;
} roast_telemetry_snapshot_t;

/** Starts the background sampling timer (500ms period). Call once at boot,
 * after max6675_init()/fan_pwm_init()/ssr_heater_init()/session_state_machine_init(). */
esp_err_t roast_telemetry_service_init(void);

/** Copies the latest cached snapshot. Cheap, safe to call every UI refresh tick. */
void roast_telemetry_service_get_snapshot(roast_telemetry_snapshot_t *out);

/** Call right after a new roast session is successfully started
 * (session_sm_start() returned ESP_OK): resets the RoR/DTR calculators and
 * opens a new session-history recording file (storage/session_store.h). */
void roast_telemetry_service_on_roast_started(void);

/** Marks the FIRST_CRACK_START event at the current elapsed roast time, so
 * DTR% starts being computed (FR-037). */
void roast_telemetry_service_mark_first_crack(void);

#ifdef __cplusplus
}
#endif

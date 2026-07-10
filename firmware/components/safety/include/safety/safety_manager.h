/**
 * @file safety_manager.h
 * @brief Centralized Safety Manager - the single gatekeeper for every
 *        fan/heater command, regardless of source (display, web, Artisan).
 *
 * Per the project constitution (Principle I, Safety-First): no component
 * other than this one may command the heater above 0% duty. All hard-fail
 * rules from research.md Decision 4 live here:
 *   1. Heater requires fan >= FAN_MIN_PCT_DURING_HEAT (30%, fixed).
 *   2. Absolute temperature cutoff at 260C (warning at 240C).
 *   3. Sensor failure -> heater forced off + critical alarm.
 *   4. Indirect fan-failure (via RoR, see fan_failure_detector.c) -> heater
 *      off + critical alarm (reported into this module by that detector).
 *   5. Max roast duration watchdog (see duration_watchdog.c) -> forces
 *      cooling (reported into this module).
 *   6. Dedicated Emergency Stop -> immediate heater off, always available.
 *   7. Critical alarms require manual acknowledgment (FR-029) before the
 *      roast may continue.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SAFETY_FAN_MIN_PCT_DURING_HEAT   30    /* FR-004: fixed, not configurable. */
#define SAFETY_TEMP_WARNING_C            240.0f /* FR-026 */
#define SAFETY_TEMP_ABSOLUTE_CUTOFF_C    260.0f /* FR-026 */
#define SAFETY_DEFAULT_MAX_DURATION_MS   (25 * 60 * 1000) /* FR-033 default 25 min. */
#define SAFETY_FAN_STOP_MIN_TEMP_C       100.0f /* Fan may only be commanded fully OFF (0%) once BT is below this - since roast profiles can now configure their own Cooling segment/duration, this stops a badly-configured (or cancelled-early) profile from cutting airflow while the heating element/chamber is still hot. */

typedef enum {
    SAFETY_ALARM_NONE = 0,
    SAFETY_ALARM_TEMP_ABSOLUTE_CUTOFF,   /* FR-026 */
    SAFETY_ALARM_SENSOR_FAILURE,         /* FR-006 */
    SAFETY_ALARM_FAN_FAILURE_INDIRECT,   /* FR-030 */
    SAFETY_ALARM_DURATION_WATCHDOG,      /* FR-033 */
    SAFETY_ALARM_EMERGENCY_STOP,         /* FR-027 */
} safety_alarm_type_t;

typedef enum {
    SAFETY_CMD_SOURCE_DISPLAY = 0,
    SAFETY_CMD_SOURCE_WEB,
    SAFETY_CMD_SOURCE_ARTISAN,
    SAFETY_CMD_SOURCE_PROFILE_CURVE,
} safety_cmd_source_t;

esp_err_t safety_manager_init(void);

/**
 * Validates and applies a fan speed request. Rejects (ESP_ERR_INVALID_STATE)
 * if the heater is currently active and pct < SAFETY_FAN_MIN_PCT_DURING_HEAT.
 */
esp_err_t safety_manager_request_fan_pct(uint8_t pct, safety_cmd_source_t source);

/**
 * Validates and applies a heater duty request. Rejects if the fan is not
 * currently at/above the safety floor, if there's an unacknowledged critical
 * alarm, or if the last temperature reading was invalid.
 */
esp_err_t safety_manager_request_heater_pct(uint8_t pct, safety_cmd_source_t source);

/** Feeds the latest validated temperature sample; drives the 260C/240C cutoff and sensor-failure checks. */
void safety_manager_on_temperature_sample(float bean_temp_c, bool sensor_valid);

/** Called by fan_failure_detector.c (FR-030) when an anomalous RoR pattern is detected during heating. */
void safety_manager_report_indirect_fan_failure(void);

/** Called by duration_watchdog.c (FR-033) when the max roast duration is exceeded. */
void safety_manager_report_duration_exceeded(void);

/** FR-022: called by session_recovery.c if the sensor is still invalid right
 * after a power-loss resume attempt, forcing the critical-alarm/mandatory-ack
 * safe state even though the heater is already off at boot (unlike
 * safety_manager_on_temperature_sample(), which only raises this alarm while
 * the heater is actively on). */
void safety_manager_report_recovery_sensor_failure(void);

/** FR-027: immediately forces heater off and raises a critical alarm, from any source (display/web). */
esp_err_t safety_manager_emergency_stop(void);

/** FR-029: acknowledges the currently active critical alarm, allowing the roast to continue. */
esp_err_t safety_manager_acknowledge_alarm(void);

/** Returns the currently active alarm (SAFETY_ALARM_NONE if none), and whether it still needs ack. */
safety_alarm_type_t safety_manager_get_active_alarm(bool *out_needs_ack);

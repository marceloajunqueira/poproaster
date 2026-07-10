/**
 * @file fan_failure_detector.h
 * @brief FR-030: indirect fan-failure detection via RoR anomaly pattern.
 *
 * There is no dedicated fan RPM sensor on this hardware (research.md
 * Decision, Session 2026-07-06 (4)), so a mechanically stuck/disconnected
 * fan is inferred indirectly: while the heater is actively on, an
 * implausibly fast bean-temperature rise (RoR) is treated as a sign that
 * air isn't actually being moved through the chamber despite the fan being
 * commanded on, since a stuck fan traps heat near the element.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the background detector timer. Call once at boot, after
 * roast_telemetry_service_init() and safety_manager_init(). */
esp_err_t fan_failure_detector_init(void);

#ifdef __cplusplus
}
#endif

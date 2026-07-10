/**
 * @file ror_calculator.h
 * @brief Rate of Rise (RoR) calculator - temperature delta per minute.
 *
 * FR-011/FR-036: displayed alongside the BT curve. Also feeds the indirect
 * fan-failure detector (FR-030, safety/fan_failure_detector.c), which looks
 * for RoR patterns inconsistent with the commanded fan speed.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct ror_calculator_s *ror_calculator_handle_t;

/** Creates a RoR calculator with a moving window (e.g. 30s) for smoothing. */
ror_calculator_handle_t ror_calculator_create(uint32_t smoothing_window_ms);

void ror_calculator_destroy(ror_calculator_handle_t handle);

/** Feeds a new temperature sample; returns false until enough history exists to compute RoR. */
bool ror_calculator_add_sample(ror_calculator_handle_t handle, float temp_c, int64_t timestamp_ms);

/** Returns the current smoothed RoR in degrees C per minute. */
float ror_calculator_get_ror_c_per_min(ror_calculator_handle_t handle);

void ror_calculator_reset(ror_calculator_handle_t handle);

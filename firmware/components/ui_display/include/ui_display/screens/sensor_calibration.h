/**
 * @file sensor_calibration.h
 * @brief T051: MAX6675 sensor calibration UI - operator provides a known
 *        reference temperature (e.g. boiling water) and the offset needed
 *        to make the live reading match it is computed and persisted via
 *        hal/max6675.h's calibration offset API (already NVS-backed).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the sensor calibration screen into `parent`. */
void sensor_calibration_show_in(lv_obj_t *parent);

/** Stops the screen's own refresh timer. */
void sensor_calibration_hide(void);

#ifdef __cplusplus
}
#endif

/**
 * @file peripheral_test.h
 * @brief T050: peripheral test screen - individual sensor/fan/heater tests,
 *        reachable from the Config tab's settings hub. Confirmation is
 *        required before the heater test (it's the only one that can
 *        actually make something hot).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the peripheral test screen into `parent`. */
void peripheral_test_show_in(lv_obj_t *parent);

/** Stops any in-progress test (forces heater/fan off) and deletes the
 * screen's own refresh timer. MUST be called both when navigating back to
 * the settings hub AND when the Config tab itself is switched away from -
 * a test must never keep running unattended once this screen isn't
 * visible anymore. */
void peripheral_test_hide(void);

#ifdef __cplusplus
}
#endif

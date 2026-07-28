/**
 * @file heater_settings.h
 * @brief Max Heater Power configuration UI - lets the operator cap the
 *        heater's PWM duty cycle (0-100%) as a hard ceiling on top of
 *        whatever the closed-loop PID/profile curve would otherwise
 *        command, via safety/safety_manager.h's
 *        safety_manager_set_max_heater_power_pct() (NVS-backed).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the Max Heater Power screen into `parent`. */
void heater_settings_show_in(lv_obj_t *parent);

/** Stops the screen's own refresh timer, if any. */
void heater_settings_hide(void);

#ifdef __cplusplus
}
#endif

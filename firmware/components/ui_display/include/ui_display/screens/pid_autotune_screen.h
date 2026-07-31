/**
 * @file pid_autotune_screen.h
 * @brief On-device relay-feedback PID autotune UI (Config menu).
 *
 * Requires an explicit "informed consent" checkbox before Start is usable -
 * the run deliberately oscillates the heater around a setpoint for several
 * minutes. Once started, the run itself lives in roast_core/pid_autotune.h
 * and profile_curve_follower.c's control tick, independent of this screen -
 * it keeps going (and auto-applies+saves its result to NVS on success) even
 * if the operator navigates away.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the PID Autotune screen into `parent`. */
void pid_autotune_screen_show_in(lv_obj_t *parent);

/** Stops the screen's own refresh timer, if any (the autotune run itself is NOT stopped). */
void pid_autotune_screen_hide(void);

#ifdef __cplusplus
}
#endif

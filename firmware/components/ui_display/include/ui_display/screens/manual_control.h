/**
 * @file manual_control.h
 * @brief T029: Manual/Artisan control screen - direct fan/heater sliders
 *        (no profile curve), rendered inside the nav_shell content pane
 *        (widgets/nav_shell.h) as the "Manual" tab.
 */
#pragma once

#include "lvgl.h"

/** Renders the manual control screen into `parent`. Matches nav_shell_show_fn_t. */
void manual_control_show_in(lv_obj_t *parent);

/** Stops the screen's own refresh timer. Matches nav_shell_hide_fn_t. */
void manual_control_hide(void);

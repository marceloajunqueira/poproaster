/**
 * @file roast_dashboard.h
 * @brief Real-time roast telemetry dashboard (T021/T063): a live BT+RoR
 *        timeline chart on top plus phase/numeric readouts (BT, RoR, DTR%,
 *        fan%, heater%, timer) and roast lifecycle controls
 *        (start/pause/resume/cooling) in a compact row below, rendered
 *        inside the nav_shell content pane (widgets/nav_shell.h). Actual
 *        sensor sampling/session recording lives in
 *        roast_core/roast_telemetry_service.h so it keeps running even
 *        while another tab is visible.
 */
#pragma once

#include "lvgl.h"

/** Renders the dashboard into `parent` (the nav_shell content pane). Matches nav_shell_show_fn_t. */
void roast_dashboard_show_in(lv_obj_t *parent);

/** Stops the dashboard's own UI-refresh timer. Matches nav_shell_hide_fn_t.
 * Background sampling/recording (roast_telemetry_service.h) is unaffected. */
void roast_dashboard_hide(void);

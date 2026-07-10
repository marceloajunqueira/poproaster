/**
 * @file session_review.h
 * @brief Session history/review screen (T022/T066): browse completed roast
 *        sessions and re-plot their BT+RoR curve, rendered inside the
 *        nav_shell content pane (widgets/nav_shell.h).
 */
#pragma once

#include "lvgl.h"

/** Renders the session list into `parent` (the nav_shell content pane). Matches nav_shell_show_fn_t. */
void session_review_show_in(lv_obj_t *parent);

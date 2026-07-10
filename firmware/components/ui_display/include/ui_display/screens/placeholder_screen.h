/**
 * @file placeholder_screen.h
 * @brief Minimal "coming soon" content-pane screen for nav_shell tabs whose
 *        real screens are not implemented yet (Presets/US2, parts of Config).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders a simple title + bullet list of upcoming features into `parent`. */
void placeholder_screen_show_in(lv_obj_t *parent, const char *title, const char *body);

#ifdef __cplusplus
}
#endif

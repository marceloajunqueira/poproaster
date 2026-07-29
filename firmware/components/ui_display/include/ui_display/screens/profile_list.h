/**
 * @file profile_list.h
 * @brief T033 (minimal): Presets tab - lists stored Roast Profiles
 *        (storage/profile_store.h) and lets the operator pick which one the
 *        next roast on the Roast tab will run.
 */
#pragma once

#include "lvgl.h"

/** Renders the profile list into `parent` (the nav_shell content pane). Matches nav_shell_show_fn_t. */
void profile_list_show_in(lv_obj_t *parent);

/** Cancels any pending "Delete confirm"/"title revert" lv_timer before the
 * screen is torn down (nav_shell tab switch-away) - without this, an armed
 * timer would fire later against dangling lv_obj_t pointers from the
 * already-lv_obj_clean()'d content pane. Matches nav_shell_hide_fn_t. */
void profile_list_hide(void);

/**
 * @file profile_editor.h
 * @brief T032: profile editor screen - combines a read-only summary of the
 *        curve (each setpoint shown as a segment "card") with an editable
 *        numeric point table (duration/target BT/target fan%/target
 *        heater%/Cooling toggle per segment), reachable from the Presets
 *        tab (ui_display/screens/profile_list.c) via "+ New" or an "Edit"
 *        button on an existing profile.
 */
#pragma once

#include "lvgl.h"

/**
 * Shows the profile editor into `parent` (the nav_shell content pane,
 * shared with profile_list.c within the Presets tab). Pass an existing
 * profile's id to edit it, or -1 to create a brand-new profile (seeded with
 * one default segment). Changes are only persisted (storage/profile_store.h)
 * when the operator taps Save; Back discards them.
 */
void profile_editor_show_in(lv_obj_t *parent, int profile_id);

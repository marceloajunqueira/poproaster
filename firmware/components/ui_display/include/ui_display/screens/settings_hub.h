/**
 * @file settings_hub.h
 * @brief T067 (partial): Config tab hub - consolidates entry points to the
 *        peripheral test (T050) and sensor calibration (T051) screens, plus
 *        pointers to the web-based Wi-Fi setup (/wifi) and firmware update
 *        (/ota) pages. Language switch/i18n catalog UI (also part of
 *        FR-046) is NOT implemented here - that depends on T055's actual
 *        catalog files (Phase 7), still pending.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the settings hub menu into `parent`. Matches nav_shell_show_fn_t - registered as the Config tab. */
void settings_hub_show_in(lv_obj_t *parent);

/** Delegates to whichever sub-screen (if any) is currently active, then clears its own state. Matches nav_shell_hide_fn_t. */
void settings_hub_hide(void);

/** Clears `parent` and rebuilds the hub menu - called by sub-screens' own "Back" buttons. */
void settings_hub_return_to_menu(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

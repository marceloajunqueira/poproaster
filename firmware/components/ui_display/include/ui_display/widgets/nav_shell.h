/**
 * @file nav_shell.h
 * @brief Persistent left navigation sidebar (Material Design style) shared by
 *        every top-level screen (Roast / Manual / Presets / History / Config),
 *        per FR-045. Also pins a permanently-visible Emergency Stop control
 *        (FR-027) so it is reachable regardless of which tab is active.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_SHELL_TAB_ROAST = 0,
    NAV_SHELL_TAB_PRESETS,
    NAV_SHELL_TAB_MANUAL,
    NAV_SHELL_TAB_HISTORY,
    NAV_SHELL_TAB_CONFIG,
    NAV_SHELL_TAB_COUNT,
} nav_shell_tab_t;

/** Renders the tab's content inside `content_parent` (the shell's content pane). */
typedef void (*nav_shell_show_fn_t)(lv_obj_t *content_parent);

/** Called right before the content pane is cleared, so the outgoing tab can
 * release any resources tied to its own UI objects (e.g. delete an
 * lv_timer_t used only for repainting - background services like
 * roast_telemetry_service.h are NOT affected by this and keep running). */
typedef void (*nav_shell_hide_fn_t)(void);

/** Registers a tab's content callbacks. Must be called for every tab BEFORE
 * nav_shell_init(). `label` must remain valid for the program's lifetime
 * (pass a string literal). */
void nav_shell_register_tab(nav_shell_tab_t tab, const char *label,
                             nav_shell_show_fn_t show_fn, nav_shell_hide_fn_t hide_fn);

/** Builds the sidebar + content pane on the active LVGL screen and shows
 * `initial_tab`. Call once at boot, after all tabs have been registered. */
esp_err_t nav_shell_init(nav_shell_tab_t initial_tab);

#ifdef __cplusplus
}
#endif

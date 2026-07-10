/**
 * @file wifi_setup.h
 * @brief T049: on-device WiFi AP + setup screen - manual SSID/password entry
 *        (no network scan list) reachable from the Config tab, so
 *        reconfiguring WiFi doesn't strictly require a phone/laptop and the
 *        web-based /wifi page (web_api/wifi_setup_routes.c already covers
 *        that - this is the on-device counterpart).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Renders the WiFi setup screen into `parent`. */
void wifi_setup_show_in(lv_obj_t *parent);

/** Stops the screen's own refresh timer. */
void wifi_setup_hide(void);

#ifdef __cplusplus
}
#endif

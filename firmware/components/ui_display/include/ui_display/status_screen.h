/**
 * @file status_screen.h
 * @brief Minimal bring-up/status screen (not the final roast dashboard -
 *        see T021 for that). Used to validate the display + touch + sensor
 *        + WiFi provisioning are all working end-to-end on real hardware.
 */
#pragma once

#include "esp_err.h"

/** Builds and shows the status screen; must be called after ui_display_panel_init(). */
esp_err_t ui_status_screen_show(void);

/**
 * @file presets_routes.h
 * @brief Web parity for the Presets tab (ui_display/screens/profile_list.c):
 *        list stored Roast Profiles and select which one the next roast
 *        runs. Read-only from the web for now (no profile editor on the
 *        web side yet - use the display's Presets tab for full CRUD,
 *        T032).
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers "/presets" (GET, list) and "/api/presets/select" (POST). Call once from web_api_server_init(). */
esp_err_t presets_routes_register(httpd_handle_t server);

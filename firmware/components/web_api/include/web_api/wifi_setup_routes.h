/**
 * @file wifi_setup_routes.h
 * @brief Minimal WiFi setup web page (T049 bring-up subset): lets the
 *        operator enter home WiFi credentials from a browser connected to
 *        the "PopRoaster-Setup" AP, without any external tool.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers the "/" (setup form or status page) and "/api/wifi/configure" routes. */
esp_err_t wifi_setup_routes_register(httpd_handle_t server);

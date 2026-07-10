/**
 * @file diagnostics_routes.h
 * @brief Web diagnostics page: free heap / internal RAM / PSRAM usage, NVS
 *        stats, per-task FreeRTOS stack high-water-mark usage, and basic
 *        system info (chip, IDF version, app build, uptime, reset reason).
 *        Read-only - no controls, purely for troubleshooting/monitoring.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers "/diagnostics" (GET). Call once from web_api_server_init(). */
esp_err_t diagnostics_routes_register(httpd_handle_t server);

/**
 * @file server.h
 * @brief Base HTTP/WebSocket server skeleton (web_api component).
 *
 * Route handlers (control commands, profile export/import, session export,
 * firmware upload, language setting) are registered by their respective
 * tasks (T042, T037/T038, T044/T048, T052, T055) against the handle exposed
 * here; the WebSocket telemetry broadcast (T041) also attaches to this server.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Starts the HTTP server and registers the routes implemented so far. */
esp_err_t web_api_server_init(void);

/** Returns the underlying esp_http_server handle so other web_api modules can register routes. */
httpd_handle_t web_api_server_get_handle(void);

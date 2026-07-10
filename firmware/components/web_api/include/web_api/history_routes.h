/**
 * @file history_routes.h
 * @brief T043 (web history parity) / T048: Roast History web pages - list +
 *        detail view (mirroring ui_display/screens/session_review.c) and a
 *        CSV session export endpoint.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers "/history" (list), "/history/detail" (?id=...), and "/api/sessions/export" (?id=...&format=csv). Call once from web_api_server_init(). */
esp_err_t history_routes_register(httpd_handle_t server);

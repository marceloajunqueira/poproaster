/**
 * @file screenshot_routes.h
 * @brief Documentation aid: serves the LCD's current contents as a BMP.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers "/api/screenshot" (GET, BMP) and "/screen" (GET, page with capture buttons). Optional
 * `?tab=roast|presets|manual|history|config` on the API switches to that tab first and restores the previous
 * one afterwards (refused with 409 while a roast is active). */
esp_err_t screenshot_routes_register(httpd_handle_t server);

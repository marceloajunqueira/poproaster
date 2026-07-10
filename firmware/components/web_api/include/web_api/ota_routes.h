/**
 * @file ota_routes.h
 * @brief T052/T053/T054: firmware update over the web UI (A/B partition
 *        OTA). Deviates from tasks.md's literal file paths
 *        (web_api/routes/firmware_upload.c + webui/src/firmware_update.html)
 *        the same way every other web_api route file in this project does -
 *        kept as one flat file (ota_routes.c) registered directly under
 *        components/web_api/, and the upload page is generated inline in C
 *        (same no-external-CDN convention as dashboard_routes.c/
 *        history_routes.c/presets_routes.c) rather than a static file under
 *        webui/src/.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Registers "/ota" (GET, upload page), "/api/ota/status" (GET, JSON), and "/api/ota/upload" (POST, raw firmware binary). */
esp_err_t ota_routes_register(httpd_handle_t server);

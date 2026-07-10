/**
 * @file ota_manager.h
 * @brief T053: A/B partition OTA update manager - thin wrapper around
 *        esp_ota_ops.h that only ever switches the boot pointer AFTER a
 *        fully written image passes esp_ota_end()'s validation (image
 *        header/checksum), and marks a freshly-booted OTA image "valid"
 *        (cancelling any pending rollback) once basic boot has succeeded.
 *
 * Rollback safety model (FR-015, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
 * already set in sdkconfig): after `ota_manager_end_and_activate()` switches
 * the boot partition and the device reboots into the new image, ESP-IDF's
 * bootloader marks that partition PENDING_VERIFY. If the device were to
 * crash-loop before `ota_manager_init()` (called early in app_main) gets a
 * chance to run, the bootloader automatically reverts to the previous
 * (known-good) partition on the next boot - no explicit crash-detection
 * logic needed here, it's the standard ESP-IDF app-rollback mechanism.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char running_version[32];        /* From the app image descriptor (esp_app_desc_t.version). */
    char running_partition_label[16];
    bool pending_verify;             /* True only right after booting a freshly-installed OTA image, before ota_manager_init() confirms it. */
} ota_manager_status_t;

/**
 * Marks the currently running app image valid (cancels any pending
 * rollback) if this is the first boot after an OTA update. Safe no-op
 * otherwise. Call once at boot, after the core subsystems that would
 * indicate a bad update have already initialized successfully.
 */
esp_err_t ota_manager_init(void);

/** Reports the running image's version/partition and whether it's still pending verification. */
esp_err_t ota_manager_get_status(ota_manager_status_t *out);

/**
 * Begins a new OTA update targeting the inactive (next) A/B partition.
 * `image_size_hint` may be 0 if the total size isn't known up front.
 * Fails with ESP_ERR_INVALID_STATE if an OTA is already in progress.
 */
esp_err_t ota_manager_begin(size_t image_size_hint);

/** Writes the next chunk of the firmware image. Must follow ota_manager_begin(). */
esp_err_t ota_manager_write(const void *data, size_t len);

/**
 * Validates the fully-written image (magic/header/checksum via
 * esp_ota_end()) and switches the boot partition to it. Returns an error
 * (and leaves the CURRENT boot partition untouched) if validation fails -
 * a corrupt/incomplete upload never becomes bootable. Does NOT reboot -
 * callers should finish responding to the HTTP request first, then call
 * ota_manager_reboot().
 */
esp_err_t ota_manager_end_and_activate(void);

/** Aborts an in-progress OTA update (write error, client disconnect, etc.) without touching the current boot partition. */
esp_err_t ota_manager_abort(void);

/** Reboots the device shortly after being called (via a one-shot timer, so an in-flight HTTP response has time to flush first). */
void ota_manager_reboot(void);

#ifdef __cplusplus
}
#endif

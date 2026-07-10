/**
 * @file ota_manager.c
 * @brief See header.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "hal/ota_manager.h"

static const char *TAG = "ota_manager";

static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;
static bool s_update_in_progress = false;

esp_err_t ota_manager_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        /* First boot after an OTA update - basic startup (this call happens
         * near the end of a successful app_main()) already succeeded, so
         * confirm this image is good and cancel the automatic rollback. */
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image on partition '%s' marked valid (rollback cancelled)", running->label);
        } else {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t ota_manager_get_status(ota_manager_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *running = esp_ota_get_running_partition();
    strlcpy(out->running_partition_label, running->label, sizeof(out->running_partition_label));

    const esp_app_desc_t *app = esp_app_get_description();
    if (app != NULL) {
        strlcpy(out->running_version, app->version, sizeof(out->running_version));
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        out->pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    return ESP_OK;
}

esp_err_t ota_manager_begin(size_t image_size_hint)
{
    if (s_update_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition available");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size_hint > 0 && image_size_hint > s_update_partition->size) {
        ESP_LOGE(TAG, "Image size %u exceeds target partition '%s' size %u",
                 (unsigned)image_size_hint, s_update_partition->label, (unsigned)s_update_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t ota_size = (image_size_hint > 0) ? image_size_hint : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(s_update_partition, ota_size, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_update_in_progress = true;
    ESP_LOGI(TAG, "OTA update started, target partition '%s' (%u bytes)",
             s_update_partition->label, (unsigned)s_update_partition->size);
    return ESP_OK;
}

esp_err_t ota_manager_write(const void *data, size_t len)
{
    if (!s_update_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_manager_end_and_activate(void)
{
    if (!s_update_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    s_update_in_progress = false;

    /* esp_ota_end() validates the image (magic bytes, header, checksum) -
     * an incomplete/corrupt upload is rejected here, BEFORE the boot
     * partition is ever touched, so the device keeps running the current
     * (known-good) firmware in that case. */
    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (image rejected): %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update validated, boot partition switched to '%s' - reboot required", s_update_partition->label);
    return ESP_OK;
}

esp_err_t ota_manager_abort(void)
{
    if (!s_update_in_progress) {
        return ESP_OK;
    }
    s_update_in_progress = false;
    esp_err_t err = esp_ota_abort(s_ota_handle);
    ESP_LOGW(TAG, "OTA update aborted");
    return err;
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void ota_manager_reboot(void)
{
    /* One-shot timer instead of calling esp_restart() immediately, so the
     * HTTP response confirming success has a chance to actually reach the
     * client before the socket/network stack goes down. */
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name = "ota_reboot",
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 800 * 1000);
    } else {
        /* Fallback: reboot immediately if even the timer couldn't be created. */
        esp_restart();
    }
}

/**
 * @file nvs_store.c
 * @brief NVS-backed key/value storage implementation.
 *
 * Single shared namespace ("poproaster") for small config-like values
 * (calibration offset, WiFi credentials, language). Roast profiles (which are
 * larger, structured JSON-like blobs) are stored via storage/profile_store.c
 * on top of these primitives; session history lives in session_store.c
 * (LittleFS), not here, since NVS is unsuitable for larger time-series data.
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#include "storage/nvs_store.h"

static const char *TAG = "nvs_store";
#define NVS_NAMESPACE "poproaster"

esp_err_t nvs_store_init(void)
{
    /* nvs_flash_init() itself is called once in main.c before any component
     * touches NVS; this function just verifies the namespace opens cleanly. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_store_set_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_get_string(const char *key, char *out_value, size_t out_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, key, out_value, &out_size);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_set_float(const char *key, float value)
{
    return nvs_store_set_blob(key, &value, sizeof(value));
}

esp_err_t nvs_store_get_float(const char *key, float *out_value)
{
    size_t len = sizeof(*out_value);
    return nvs_store_get_blob(key, out_value, &len);
}

esp_err_t nvs_store_set_i32(const char *key, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_get_i32(const char *key, int32_t *out_value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_i32(handle, key, out_value);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_set_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    } else if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGE(TAG, "NVS full while writing key '%s' (%d bytes)", key, (int)len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_get_blob(const char *key, void *out_data, size_t *inout_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(handle, key, out_data, inout_len);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_erase_key(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

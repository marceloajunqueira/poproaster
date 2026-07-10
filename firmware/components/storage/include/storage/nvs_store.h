/**
 * @file nvs_store.h
 * @brief NVS-backed key/value storage for profiles, calibration, network and language config.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t nvs_store_init(void);

esp_err_t nvs_store_set_string(const char *key, const char *value);
esp_err_t nvs_store_get_string(const char *key, char *out_value, size_t out_size);

esp_err_t nvs_store_set_float(const char *key, float value);
esp_err_t nvs_store_get_float(const char *key, float *out_value);

esp_err_t nvs_store_set_i32(const char *key, int32_t value);
esp_err_t nvs_store_get_i32(const char *key, int32_t *out_value);

esp_err_t nvs_store_set_blob(const char *key, const void *data, size_t len);
esp_err_t nvs_store_get_blob(const char *key, void *out_data, size_t *inout_len);

esp_err_t nvs_store_erase_key(const char *key);

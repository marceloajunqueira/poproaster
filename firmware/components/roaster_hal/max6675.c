/**
 * @file max6675.c
 * @brief MAX6675 thermocouple amplifier driver implementation.
 *
 * The MAX6675 is read-only over a 3-wire SPI-like bus (SCK, CS, SO). It has
 * no MOSI line (the MCU never writes to it), which is why SO is safely wired
 * to IO46 (input-only on ESP32-S3) - see board_config.h.
 *
 * Protocol: pulling CS low starts a conversion read; 16 clock cycles are
 * shifted out on SO, MSB first. Bit 2 (D2) is the "open thermocouple" flag.
 * The upper 12 bits (D14..D3) are the temperature in 0.25C steps.
 */
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "hal/max6675.h"
#include "storage/nvs_store.h"

static const char *TAG = "max6675";

#define MAX6675_HOST         SPI2_HOST
#define MAX6675_CLOCK_HZ     (1 * 1000 * 1000) /* MAX6675 max SCK is ~4.3MHz; 1MHz is a safe margin. */
#define MAX6675_STALE_MS     2000              /* No fresh conversion in this window => STALE. */
#define MAX6675_MIN_PLAUSIBLE_C (-10.0f)
#define MAX6675_MAX_PLAUSIBLE_C (350.0f)

static spi_device_handle_t s_spi_dev;
static float s_calibration_offset_c = 0.0f;
static int64_t s_last_valid_read_ms = 0;
static bool s_initialized = false;

esp_err_t max6675_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Restore any previously saved calibration offset (FR-032). */
    float saved_offset = 0.0f;
    if (nvs_store_get_float("max6675_offset", &saved_offset) == ESP_OK) {
        s_calibration_offset_c = saved_offset;
    }

    spi_bus_config_t bus_cfg = {
        .miso_io_num = BOARD_PERIPH_MAX6675_SO_GPIO,
        .mosi_io_num = -1, /* MAX6675 has no data-in line. */
        .sclk_io_num = BOARD_PERIPH_MAX6675_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    esp_err_t err = spi_bus_initialize(MAX6675_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = MAX6675_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BOARD_PERIPH_MAX6675_CS_GPIO,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(MAX6675_HOST, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MAX6675 init OK (SCK=%d CS=%d SO=%d, calibration offset=%.2fC)",
             BOARD_PERIPH_MAX6675_SCK_GPIO, BOARD_PERIPH_MAX6675_CS_GPIO,
             BOARD_PERIPH_MAX6675_SO_GPIO, s_calibration_offset_c);
    return ESP_OK;
}

esp_err_t max6675_read(max6675_sample_t *out_sample)
{
    if (out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_sample->timestamp_ms = esp_timer_get_time() / 1000;

    if (!s_initialized) {
        out_sample->bean_temp_c = 0.0f;
        out_sample->quality = MAX6675_QUALITY_DISCONNECTED;
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .rxlength = 16, /* Read-only: MAX6675 has no MOSI, so only the RX phase is enabled (half-duplex). */
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_device_transmit failed: %s", esp_err_to_name(err));
        out_sample->bean_temp_c = 0.0f;
        out_sample->quality = MAX6675_QUALITY_DISCONNECTED;
        return err;
    }

    uint16_t raw = ((uint16_t)rx[0] << 8) | rx[1];
    bool thermocouple_open = (raw & 0x0004) != 0;
    uint16_t counts = (raw >> 3) & 0x0FFF; /* 12-bit value, 0.25C per count. */
    float raw_temp_c = counts * 0.25f;
    float calibrated_temp_c = raw_temp_c + s_calibration_offset_c;

    if (thermocouple_open) {
        out_sample->bean_temp_c = 0.0f;
        out_sample->quality = MAX6675_QUALITY_DISCONNECTED;
        return ESP_OK;
    }

    if (calibrated_temp_c < MAX6675_MIN_PLAUSIBLE_C || calibrated_temp_c > MAX6675_MAX_PLAUSIBLE_C) {
        out_sample->bean_temp_c = calibrated_temp_c;
        out_sample->quality = MAX6675_QUALITY_OUT_OF_RANGE;
        return ESP_OK;
    }

    int64_t now_ms = out_sample->timestamp_ms;
    if (s_last_valid_read_ms != 0 && (now_ms - s_last_valid_read_ms) > MAX6675_STALE_MS) {
        out_sample->bean_temp_c = calibrated_temp_c;
        out_sample->quality = MAX6675_QUALITY_STALE;
        s_last_valid_read_ms = now_ms;
        return ESP_OK;
    }

    s_last_valid_read_ms = now_ms;
    out_sample->bean_temp_c = calibrated_temp_c;
    out_sample->quality = MAX6675_QUALITY_VALID;
    return ESP_OK;
}

esp_err_t max6675_set_calibration_offset(float offset_c)
{
    s_calibration_offset_c = offset_c;
    return nvs_store_set_float("max6675_offset", offset_c);
}

float max6675_get_calibration_offset(void)
{
    return s_calibration_offset_c;
}

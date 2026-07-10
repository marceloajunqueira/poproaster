/**
 * @file max6675.h
 * @brief MAX6675 thermocouple amplifier driver (BT sensor).
 *
 * Pins come from board_config.h (BOARD_PERIPH_MAX6675_*), which are
 * configurable via `idf.py menuconfig` -> Pop Roaster Board Configuration.
 * Applies a calibration offset (FR-032) on top of the raw reading.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    MAX6675_QUALITY_VALID = 0,
    MAX6675_QUALITY_STALE,
    MAX6675_QUALITY_OUT_OF_RANGE,
    MAX6675_QUALITY_DISCONNECTED,
} max6675_quality_t;

typedef struct {
    float bean_temp_c;          /* Calibrated reading (raw + offset). */
    max6675_quality_t quality;  /* FR-006/FR-030: used to decide safety cutoffs. */
    int64_t timestamp_ms;
} max6675_sample_t;

/** Initializes the SPI-like bit-bang/hw-SPI interface to the MAX6675. */
esp_err_t max6675_init(void);

/** Reads the current temperature; always succeeds, quality reflects validity. */
esp_err_t max6675_read(max6675_sample_t *out_sample);

/**
 * Sets the calibration offset in Celsius, applied to every subsequent read
 * until reconfigured (FR-032). Typically derived from a known reference
 * (e.g. boiling water at 100C) via the peripheral test screen.
 */
esp_err_t max6675_set_calibration_offset(float offset_c);

/** Returns the currently configured calibration offset in Celsius. */
float max6675_get_calibration_offset(void);

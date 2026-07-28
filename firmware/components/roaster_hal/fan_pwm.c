/**
 * @file fan_pwm.c
 * @brief Fan PWM control implementation using the ESP-IDF LEDC peripheral.
 */
#include "driver/ledc.h"
#include "esp_log.h"
#include <stdbool.h>

#include "board_config.h"
#include "hal/fan_pwm.h"

static const char *TAG = "fan_pwm";

/* Operator report: the fan motor doesn't reliably spin below ~65% duty (30%
 * was too weak) - clamp any nonzero request up to this floor. This is a
 * hardware/motor characteristic, independent of (and lower-bounds) the
 * Safety Manager's own SAFETY_FAN_MIN_PCT_DURING_HEAT floor. */
#define FAN_MIN_OPERATING_PCT 65

/* Operator report: the fan turning on instantly (0 -> target duty) pulls
 * enough inrush current to stress the power supply - ramp up smoothly over
 * this many milliseconds whenever the fan turns ON from a full stop. Turning
 * OFF (or adjusting an already-running fan's speed) stays instantaneous. */
#define FAN_SOFT_START_MS 3000

static uint8_t s_fan_pct = 0;

esp_err_t fan_pwm_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = (ledc_timer_t)BOARD_PERIPH_FAN_PWM_LEDC_TIMER,
        .freq_hz = BOARD_PERIPH_FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = BOARD_PERIPH_FAN_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL,
        .timer_sel = (ledc_timer_t)BOARD_PERIPH_FAN_PWM_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Needed for the soft-start ramp (ledc_set_fade_with_time/ledc_fade_start) below. */
    err = ledc_fade_func_install(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_fade_func_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_fan_pct = 0;
    ESP_LOGI(TAG, "Fan PWM init OK (GPIO=%d, timer=%d, channel=%d, freq=%dHz, min=%d%%, soft-start=%dms)",
             BOARD_PERIPH_FAN_PWM_GPIO, BOARD_PERIPH_FAN_PWM_LEDC_TIMER,
             BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, BOARD_PERIPH_FAN_PWM_FREQ_HZ,
             FAN_MIN_OPERATING_PCT, FAN_SOFT_START_MS);
    return ESP_OK;
}

esp_err_t fan_pwm_set_pct(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    if (pct > 0 && pct < FAN_MIN_OPERATING_PCT) {
        pct = FAN_MIN_OPERATING_PCT;
    }
    uint32_t max_duty = (1 << 10) - 1; /* 10-bit resolution. */
    uint32_t duty = (max_duty * pct) / 100;

    bool turning_on_from_stop = (s_fan_pct == 0 && pct > 0);

    esp_err_t err;
    if (turning_on_from_stop) {
        err = ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL,
                                       duty, FAN_SOFT_START_MS);
        if (err != ESP_OK) {
            return err;
        }
        err = ledc_fade_start(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL,
                               LEDC_FADE_NO_WAIT);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, duty);
        if (err != ESP_OK) {
            return err;
        }
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_fan_pct = pct;
    return ESP_OK;
}

uint8_t fan_pwm_get_pct(void)
{
    return s_fan_pct;
}

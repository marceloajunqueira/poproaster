/**
 * @file fan_pwm.c
 * @brief Fan PWM control implementation using the ESP-IDF LEDC peripheral.
 */
#include "driver/ledc.h"
#include "esp_log.h"

#include "board_config.h"
#include "hal/fan_pwm.h"

static const char *TAG = "fan_pwm";

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

    s_fan_pct = 0;
    ESP_LOGI(TAG, "Fan PWM init OK (GPIO=%d, timer=%d, channel=%d, freq=%dHz)",
             BOARD_PERIPH_FAN_PWM_GPIO, BOARD_PERIPH_FAN_PWM_LEDC_TIMER,
             BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, BOARD_PERIPH_FAN_PWM_FREQ_HZ);
    return ESP_OK;
}

esp_err_t fan_pwm_set_pct(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    uint32_t max_duty = (1 << 10) - 1; /* 10-bit resolution. */
    uint32_t duty = (max_duty * pct) / 100;

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
    if (err != ESP_OK) {
        return err;
    }

    s_fan_pct = pct;
    return ESP_OK;
}

uint8_t fan_pwm_get_pct(void)
{
    return s_fan_pct;
}

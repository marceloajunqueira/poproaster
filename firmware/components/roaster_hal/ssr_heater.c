/**
 * @file ssr_heater.c
 * @brief SSR heater control implementation (software time-proportioning).
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "hal/ssr_heater.h"

static const char *TAG = "ssr_heater";

/* 2-second window is a common, SSR-friendly time-proportioning period for
 * resistive heating loads (avoids excessive mechanical/thermal cycling of
 * the relay while still giving reasonably fine-grained average power control). */
#define SSR_WINDOW_MS 2000

static uint8_t s_duty_pct = 0;
static esp_timer_handle_t s_window_timer;
static bool s_gpio_state = false;

static void ssr_set_gpio(bool on)
{
    s_gpio_state = on;
    gpio_set_level(BOARD_PERIPH_SSR_HEATER_GPIO, on ? 1 : 0);
}

static void ssr_window_cb(void *arg)
{
    /* Simple software time-proportioning: ON for duty_pct% of the window,
     * OFF for the rest. Re-armed every window via esp_timer periodic mode. */
    static uint32_t elapsed_in_window_ms = 0;
    elapsed_in_window_ms += 50;
    if (elapsed_in_window_ms >= SSR_WINDOW_MS) {
        elapsed_in_window_ms = 0;
    }
    uint32_t on_time_ms = (SSR_WINDOW_MS * s_duty_pct) / 100;
    ssr_set_gpio(elapsed_in_window_ms < on_time_ms);
}

esp_err_t ssr_heater_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_PERIPH_SSR_HEATER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* Default-safe: heater OFF if pin floats during boot. */
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    ssr_set_gpio(false);
    s_duty_pct = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = &ssr_window_cb,
        .name = "ssr_window",
    };
    err = esp_timer_create(&timer_args, &s_window_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 50ms tick resolution for the time-proportioning window. */
    err = esp_timer_start_periodic(s_window_timer, 50 * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SSR heater init OK (GPIO=%d, window=%dms)", BOARD_PERIPH_SSR_HEATER_GPIO, SSR_WINDOW_MS);
    return ESP_OK;
}

esp_err_t ssr_heater_set_duty_pct(uint8_t duty_pct)
{
    if (duty_pct > 100) {
        duty_pct = 100;
    }
    s_duty_pct = duty_pct;
    if (duty_pct == 0) {
        ssr_set_gpio(false);
    }
    return ESP_OK;
}

uint8_t ssr_heater_get_duty_pct(void)
{
    return s_duty_pct;
}

esp_err_t ssr_heater_force_off(void)
{
    s_duty_pct = 0;
    ssr_set_gpio(false);
    return ESP_OK;
}

uint32_t ssr_heater_get_window_ms(void)
{
    return SSR_WINDOW_MS;
}

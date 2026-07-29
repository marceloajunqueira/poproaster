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

/* Time-proportioning period for the resistive heating element.
 *
 * ORIGINALLY 2000ms, reduced to 500ms after an operator report: "a
 * resistencia fica vermelha e apaga, vermelha e apaga, parece que ela fica
 * em um ciclo lento de liga e desliga". That was exactly right - with a 2s
 * window, a 60% duty means the coil sits fully ON for 1.2s then fully OFF
 * for 0.8s, over and over. On this hardware (bare coil in a tube with the
 * fan blowing through it, hot-air-gun style) the coil has very little
 * thermal mass, so it visibly glows and goes dark each cycle AND the air
 * temperature leaving the tube swings with it - injecting a periodic
 * temperature ripple into the very signal the PID is trying to regulate.
 * At 500ms the coil never fully cools between pulses, so the delivered heat
 * is much closer to a true continuous average.
 *
 * NOTE: nothing about the switching rate was changed by the PID retune -
 * this window has been 2000ms since the driver was first written; the
 * slow visible cycling was always there, it just became noticeable once
 * the PID stopped pinning the duty at 100% all the time. */
#define SSR_WINDOW_MS 500

/* Tick resolution of the time-proportioning window. Must divide the window
 * finely enough to keep duty granularity usable: at a 10ms tick a 500ms
 * window gives 50 discrete steps (2% duty granularity). The previous 50ms
 * tick would have left only 10 steps (10% granularity) at this window
 * size, so it was reduced alongside the window. 10ms is also comfortably
 * longer than one mains half-cycle (8.3ms @60Hz), so a zero-cross SSR can
 * still track it. */
#define SSR_TICK_MS 10

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
     * OFF for the rest. Position within the window is derived from the
     * monotonic timer rather than an incrementing counter, so it stays
     * correct regardless of tick period and can't drift if a tick is
     * delayed. */
    (void)arg;
    uint32_t pos_in_window_ms = (uint32_t)((esp_timer_get_time() / 1000) % SSR_WINDOW_MS);
    uint32_t on_time_ms = (SSR_WINDOW_MS * s_duty_pct) / 100;
    ssr_set_gpio(pos_in_window_ms < on_time_ms);
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
    /* Tick resolution for the time-proportioning window. */
    err = esp_timer_start_periodic(s_window_timer, SSR_TICK_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SSR heater init OK (GPIO=%d, window=%dms, tick=%dms)", BOARD_PERIPH_SSR_HEATER_GPIO,
             SSR_WINDOW_MS, SSR_TICK_MS);
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

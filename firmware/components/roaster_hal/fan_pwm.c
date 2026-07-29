/**
 * @file fan_pwm.c
 * @brief Fan PWM control implementation using the ESP-IDF LEDC peripheral.
 */
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>

#include "board_config.h"
#include "hal/fan_pwm.h"

static const char *TAG = "fan_pwm";

/* Operator report: the fan motor only behaves predictably at a handful of
 * discrete speeds, not arbitrary percentages - 0=off, 1..5 map to these
 * fixed duty values. This is a hardware/motor characteristic, independent
 * of (and lower-bounds) the Safety Manager's own
 * SAFETY_FAN_MIN_PCT_DURING_HEAT floor. */
static const uint8_t FAN_LEVEL_PCT[FAN_PWM_LEVEL_COUNT] = {0, 60, 70, 80, 90, 100};

/* Operator report: the fan turning on instantly (0 -> target duty) pulls
 * enough inrush current to stress the power supply - ramp up smoothly over
 * this many milliseconds whenever the fan turns ON from a full stop. Turning
 * OFF (or adjusting an already-running fan's speed) stays instantaneous.
 * CRITICAL SAFETY NOTE: while ramping, fan_pwm_get_pct() reports an
 * INTERPOLATED (not instantly-jumped-to-target) value - see below - so the
 * Safety Manager's "heater needs fan >= floor" check can't be satisfied
 * until the fan has ACTUALLY (not just nominally) reached that speed;
 * without this, the heater could start before the fan physically spun up,
 * risking the element overheating with insufficient real airflow
 * (operator-reported risk). */
#define FAN_SOFT_START_MS 2000

static uint8_t s_fan_target_pct = 0;   /* Last requested (post-quantization) percentage. */
static bool s_fan_ramping = false;     /* True while a soft-start ramp toward s_fan_target_pct is in progress. */
static int64_t s_fan_ramp_start_ms = 0;
static uint32_t s_fan_ramp_from_pct = 0;

uint8_t fan_pwm_level_to_pct(uint8_t level)
{
    if (level > FAN_PWM_LEVEL_MAX) {
        level = FAN_PWM_LEVEL_MAX;
    }
    return FAN_LEVEL_PCT[level];
}

uint8_t fan_pwm_pct_to_level(uint8_t pct)
{
    uint8_t best_level = 0;
    int best_diff = 1000;
    for (uint8_t i = 0; i < FAN_PWM_LEVEL_COUNT; i++) {
        int diff = (int)pct - (int)FAN_LEVEL_PCT[i];
        if (diff < 0) {
            diff = -diff;
        }
        if (diff < best_diff) {
            best_diff = diff;
            best_level = i;
        }
    }
    return best_level;
}

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

    s_fan_target_pct = 0;
    s_fan_ramping = false;
    ESP_LOGI(TAG, "Fan PWM init OK (GPIO=%d, timer=%d, channel=%d, freq=%dHz, levels=0/60/70/80/90/100%%, soft-start=%dms)",
             BOARD_PERIPH_FAN_PWM_GPIO, BOARD_PERIPH_FAN_PWM_LEDC_TIMER,
             BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, BOARD_PERIPH_FAN_PWM_FREQ_HZ,
             FAN_SOFT_START_MS);
    return ESP_OK;
}

esp_err_t fan_pwm_set_pct(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    if (pct > 0) {
        /* Snap to the nearest discrete level; a deliberate nonzero request
         * must never round down to "off" (level 0), so it's floored at the
         * lowest nonzero level (60%) instead. */
        uint8_t level = fan_pwm_pct_to_level(pct);
        if (level < 1) {
            level = 1;
        }
        pct = fan_pwm_level_to_pct(level);
    }
    uint32_t max_duty = (1 << 10) - 1; /* 10-bit resolution. */
    uint32_t duty = (max_duty * pct) / 100;

    /* Use the CURRENT REAL (possibly still-ramping) percentage, not just
     * the last commanded target, to decide whether this is a fresh "turn
     * on from a stop" - if a previous ramp toward a nonzero target hasn't
     * finished yet and a new nonzero level is requested mid-ramp, this is
     * an adjustment, not a fresh start (avoids restarting a full 2s ramp
     * from 0 every time the operator nudges the level up/down quickly). */
    uint8_t current_real_pct = fan_pwm_get_pct();
    bool turning_on_from_stop = (current_real_pct == 0 && pct > 0);

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
        s_fan_ramping = true;
        s_fan_ramp_start_ms = esp_timer_get_time() / 1000;
        s_fan_ramp_from_pct = 0;
    } else {
        /* Abort any still-running fade first - a plain ledc_set_duty()
         * while a hardware fade is active is not guaranteed to take
         * effect immediately. */
        ledc_fade_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
        err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, duty);
        if (err != ESP_OK) {
            return err;
        }
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
        if (err != ESP_OK) {
            return err;
        }
        s_fan_ramping = false;
    }

    s_fan_target_pct = pct;
    return ESP_OK;
}

uint8_t fan_pwm_get_pct(void)
{
    if (!s_fan_ramping) {
        return s_fan_target_pct;
    }
    int64_t elapsed_ms = (esp_timer_get_time() / 1000) - s_fan_ramp_start_ms;
    if (elapsed_ms >= FAN_SOFT_START_MS) {
        /* Ramp has naturally completed - stop interpolating from now on. */
        s_fan_ramping = false;
        return s_fan_target_pct;
    }
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }
    /* Linear interpolation matching the hardware's own linear LEDC fade -
     * this is what the fan is ACTUALLY (approximately) doing right now,
     * not just the eventual target - critical so the Safety Manager's
     * "fan must be >= floor before heater can turn on" check can't be
     * satisfied by a fan that's still spinning up. */
    uint32_t span = (uint32_t)s_fan_target_pct - s_fan_ramp_from_pct;
    uint32_t interpolated = s_fan_ramp_from_pct + (uint32_t)(((uint64_t)span * (uint32_t)elapsed_ms) / FAN_SOFT_START_MS);
    return (uint8_t)interpolated;
}

uint8_t fan_pwm_get_target_pct(void)
{
    return s_fan_target_pct;
}

esp_err_t fan_pwm_force_off(void)
{
    /* Emergency Stop only: abort any in-progress soft-start fade first (a
     * plain ledc_set_duty() while a fade is active is not guaranteed to
     * take effect), then cut the duty to 0 immediately - no fade, no level
     * quantization, no delay. */
    ledc_fade_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL, 0);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL);
    }
    s_fan_target_pct = 0;
    s_fan_ramping = false;
    return err;
}

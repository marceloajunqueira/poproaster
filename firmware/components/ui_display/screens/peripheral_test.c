/**
 * @file peripheral_test.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "hal/max6675.h"
#include "hal/fan_pwm.h"
#include "hal/ssr_heater.h"
#include "roast_core/command_dispatcher.h"
#include "safety/safety_manager.h"
#include "ui_display/screens/settings_hub.h"
#include "ui_display/screens/peripheral_test.h"

static const char *TAG = "peripheral_test";

#define FAN_TEST_PCT        50
#define FAN_TEST_MS         5000
#define HEATER_TEST_PCT     20  /* Deliberately low - this is a functional smoke test (does the SSR/element respond at all), not a real roast. */
#define HEATER_TEST_MS      8000
#define CONFIRM_TIMEOUT_MS  4000

static lv_obj_t *s_sensor_label;
static lv_obj_t *s_fan_label;
static lv_obj_t *s_heater_label;
static lv_obj_t *s_fan_btn_label;
static lv_obj_t *s_heater_btn_label;
static lv_timer_t *s_refresh_timer;
static lv_timer_t *s_fan_test_timer;
static lv_timer_t *s_heater_test_timer;
static lv_timer_t *s_heater_confirm_timer;
static bool s_fan_test_running = false;
static bool s_heater_test_running = false;
static bool s_heater_confirm_armed = false;

static lv_style_t s_style_title;
static lv_style_t s_style_label;
static bool s_styles_ready = false;

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, lv_color_hex(0xe0e0e0));
    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, lv_color_hex(0x9e9e9e));
    s_styles_ready = true;
}

static void stop_fan_test(void)
{
    if (s_fan_test_timer != NULL) {
        lv_timer_del(s_fan_test_timer);
        s_fan_test_timer = NULL;
    }
    if (s_fan_test_running) {
        command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        s_fan_test_running = false;
    }
    if (s_fan_btn_label != NULL) {
        lv_label_set_text(s_fan_btn_label, "Test Fan (50% / 5s)");
    }
}

static void stop_heater_test(void)
{
    if (s_heater_test_timer != NULL) {
        lv_timer_del(s_heater_test_timer);
        s_heater_test_timer = NULL;
    }
    if (s_heater_confirm_timer != NULL) {
        lv_timer_del(s_heater_confirm_timer);
        s_heater_confirm_timer = NULL;
    }
    s_heater_confirm_armed = false;
    if (s_heater_test_running) {
        /* Heater first (Safety Manager requires it off before fan can ever
         * drop below the floor), fan afterwards. */
        command_dispatcher_set_heater_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        s_heater_test_running = false;
    }
    if (s_heater_btn_label != NULL) {
        lv_label_set_text(s_heater_btn_label, "Test Heater");
    }
}

static void fan_test_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    s_fan_test_timer = NULL;
    stop_fan_test();
}

static void heater_test_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    s_heater_test_timer = NULL;
    stop_heater_test();
}

static void heater_confirm_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    s_heater_confirm_timer = NULL;
    s_heater_confirm_armed = false;
    if (s_heater_btn_label != NULL) {
        lv_label_set_text(s_heater_btn_label, "Test Heater");
    }
}

static void fan_test_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_fan_test_running) {
        stop_fan_test(); /* Tapping again while running cancels early. */
        return;
    }
    esp_err_t err = command_dispatcher_set_fan_pct(FAN_TEST_PCT, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Fan test rejected: %s", esp_err_to_name(err));
        return;
    }
    s_fan_test_running = true;
    lv_label_set_text(s_fan_btn_label, "Running... (tap to stop)");
    if (s_fan_test_timer != NULL) {
        lv_timer_del(s_fan_test_timer);
    }
    s_fan_test_timer = lv_timer_create(fan_test_timeout_cb, FAN_TEST_MS, NULL);
    lv_timer_set_repeat_count(s_fan_test_timer, 1);
}

/* Two-tap confirm (same pattern as session_review.c's "Delete All") since
 * this is the one test that commands the heater - never fire on a single
 * accidental tap. */
static void heater_test_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_heater_test_running) {
        stop_heater_test(); /* Tapping again while running cancels early. */
        return;
    }
    if (!s_heater_confirm_armed) {
        s_heater_confirm_armed = true;
        lv_label_set_text(s_heater_btn_label, "Confirm?");
        if (s_heater_confirm_timer != NULL) {
            lv_timer_del(s_heater_confirm_timer);
        }
        s_heater_confirm_timer = lv_timer_create(heater_confirm_timeout_cb, CONFIRM_TIMEOUT_MS, NULL);
        lv_timer_set_repeat_count(s_heater_confirm_timer, 1);
        return;
    }

    if (s_heater_confirm_timer != NULL) {
        lv_timer_del(s_heater_confirm_timer);
        s_heater_confirm_timer = NULL;
    }
    s_heater_confirm_armed = false;

    /* Fan must be at/above the safety floor BEFORE the heater request, or
     * command_dispatcher/safety_manager rejects it (FR-004). */
    esp_err_t err = command_dispatcher_set_fan_pct(SAFETY_FAN_MIN_PCT_DURING_HEAT, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Heater test: fan floor rejected: %s", esp_err_to_name(err));
        lv_label_set_text(s_heater_btn_label, "Test Heater");
        return;
    }
    err = command_dispatcher_set_heater_pct(HEATER_TEST_PCT, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Heater test rejected: %s", esp_err_to_name(err));
        command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
        lv_label_set_text(s_heater_btn_label, "Test Heater");
        return;
    }

    s_heater_test_running = true;
    lv_label_set_text(s_heater_btn_label, "Running... (tap to stop)");
    if (s_heater_test_timer != NULL) {
        lv_timer_del(s_heater_test_timer);
    }
    s_heater_test_timer = lv_timer_create(heater_test_timeout_cb, HEATER_TEST_MS, NULL);
    lv_timer_set_repeat_count(s_heater_test_timer, 1);
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    peripheral_test_hide();
    lv_obj_clean(parent);
    settings_hub_return_to_menu(parent);
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    max6675_sample_t sample;
    char buf[64];
    if (max6675_read(&sample) == ESP_OK && sample.quality == MAX6675_QUALITY_VALID) {
        snprintf(buf, sizeof(buf), "Sensor: %.1f C (Valid)", sample.bean_temp_c);
    } else {
        const char *quality_str = "Invalid";
        switch (sample.quality) {
        case MAX6675_QUALITY_STALE: quality_str = "Stale"; break;
        case MAX6675_QUALITY_OUT_OF_RANGE: quality_str = "Out of range"; break;
        case MAX6675_QUALITY_DISCONNECTED: quality_str = "Disconnected"; break;
        default: break;
        }
        snprintf(buf, sizeof(buf), "Sensor: -- (%s)", quality_str);
    }
    lv_label_set_text(s_sensor_label, buf);

    snprintf(buf, sizeof(buf), "Fan: %d%%", fan_pwm_get_pct());
    lv_label_set_text(s_fan_label, buf);

    snprintf(buf, sizeof(buf), "Heater: %d%%", ssr_heater_get_duty_pct());
    lv_label_set_text(s_heater_label, buf);
}

void peripheral_test_show_in(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Peripheral Test");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, parent);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    s_sensor_label = lv_label_create(parent);
    lv_obj_add_style(s_sensor_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_sensor_label, "Sensor: --");
    lv_obj_align(s_sensor_label, LV_ALIGN_TOP_LEFT, 12, 44);

    /* Fan test row */
    s_fan_label = lv_label_create(parent);
    lv_obj_add_style(s_fan_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_fan_label, "Fan: 0%");
    lv_obj_align(s_fan_label, LV_ALIGN_TOP_LEFT, 12, 76);

    lv_obj_t *fan_btn = lv_btn_create(parent);
    lv_obj_set_size(fan_btn, content_w - 24, 30);
    lv_obj_align(fan_btn, LV_ALIGN_TOP_LEFT, 12, 100);
    lv_obj_add_event_cb(fan_btn, fan_test_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_fan_btn_label = lv_label_create(fan_btn);
    lv_label_set_text(s_fan_btn_label, "Test Fan (50% / 5s)");
    lv_obj_center(s_fan_btn_label);

    /* Heater test row */
    s_heater_label = lv_label_create(parent);
    lv_obj_add_style(s_heater_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_heater_label, "Heater: 0%");
    lv_obj_align(s_heater_label, LV_ALIGN_TOP_LEFT, 12, 140);

    lv_obj_t *heater_btn = lv_btn_create(parent);
    lv_obj_set_size(heater_btn, content_w - 24, 30);
    lv_obj_align(heater_btn, LV_ALIGN_TOP_LEFT, 12, 164);
    lv_obj_set_style_bg_color(heater_btn, lv_color_hex(0xB3261E), LV_PART_MAIN);
    lv_obj_add_event_cb(heater_btn, heater_test_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_heater_btn_label = lv_label_create(heater_btn);
    lv_label_set_text(s_heater_btn_label, "Test Heater");
    lv_obj_center(s_heater_btn_label);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, content_w - 24);
    lv_label_set_text(note, "Heater test requires confirmation, auto-raises fan to the 30% floor, and auto-stops after 8s. Leaving this screen always stops any running test.");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 12, 200);

    /* Reset any stale state from a previous visit. */
    s_fan_test_running = false;
    s_heater_test_running = false;
    s_heater_confirm_armed = false;

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "Peripheral test screen shown");
}

void peripheral_test_hide(void)
{
    stop_fan_test();
    stop_heater_test();
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    s_sensor_label = NULL;
    s_fan_label = NULL;
    s_heater_label = NULL;
    s_fan_btn_label = NULL;
    s_heater_btn_label = NULL;
}

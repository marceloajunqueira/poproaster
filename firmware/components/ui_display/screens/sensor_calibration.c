/**
 * @file sensor_calibration.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "hal/max6675.h"
#include "ui_display/screens/settings_hub.h"
#include "ui_display/screens/sensor_calibration.h"

static const char *TAG = "sensor_calibration";

#define REF_TEMP_MIN_C 0.0f
#define REF_TEMP_MAX_C 260.0f
#define REF_TEMP_STEP_C 1.0f

static lv_obj_t *s_reading_label;
static lv_obj_t *s_offset_label;
static lv_obj_t *s_ref_label;
static lv_timer_t *s_refresh_timer;
static float s_ref_temp_c = 100.0f; /* Default reference: boiling water at sea level. */

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

static void update_ref_label(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Reference: %.0f C", (double)s_ref_temp_c);
    lv_label_set_text(s_ref_label, buf);
}

static void ref_minus_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_ref_temp_c -= REF_TEMP_STEP_C;
    if (s_ref_temp_c < REF_TEMP_MIN_C) {
        s_ref_temp_c = REF_TEMP_MIN_C;
    }
    update_ref_label();
}

static void ref_plus_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_ref_temp_c += REF_TEMP_STEP_C;
    if (s_ref_temp_c > REF_TEMP_MAX_C) {
        s_ref_temp_c = REF_TEMP_MAX_C;
    }
    update_ref_label();
}

/* Applies a new calibration offset such that the NEXT reading equals the
 * chosen reference temperature: raw = current_calibrated - old_offset;
 * new_offset = ref_temp - raw. */
static void apply_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    max6675_sample_t sample;
    if (max6675_read(&sample) != ESP_OK || sample.quality != MAX6675_QUALITY_VALID) {
        ESP_LOGW(TAG, "Cannot calibrate: sensor reading not valid right now");
        return;
    }
    float old_offset = max6675_get_calibration_offset();
    float raw_temp_c = sample.bean_temp_c - old_offset;
    float new_offset = s_ref_temp_c - raw_temp_c;
    esp_err_t err = max6675_set_calibration_offset(new_offset);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist calibration offset: %s", esp_err_to_name(err));
    }
}

static void reset_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    max6675_set_calibration_offset(0.0f);
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    sensor_calibration_hide();
    lv_obj_clean(parent);
    settings_hub_return_to_menu(parent);
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    max6675_sample_t sample;
    char buf[64];
    if (max6675_read(&sample) == ESP_OK && sample.quality == MAX6675_QUALITY_VALID) {
        snprintf(buf, sizeof(buf), "Live reading: %.1f C", sample.bean_temp_c);
    } else {
        snprintf(buf, sizeof(buf), "Live reading: --");
    }
    lv_label_set_text(s_reading_label, buf);

    snprintf(buf, sizeof(buf), "Current offset: %+.1f C", (double)max6675_get_calibration_offset());
    lv_label_set_text(s_offset_label, buf);
}

void sensor_calibration_show_in(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Sensor Calibration");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, parent);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    s_reading_label = lv_label_create(parent);
    lv_obj_add_style(s_reading_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_reading_label, "Live reading: --");
    lv_obj_align(s_reading_label, LV_ALIGN_TOP_LEFT, 12, 44);

    s_offset_label = lv_label_create(parent);
    lv_obj_add_style(s_offset_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_offset_label, "Current offset: +0.0 C");
    lv_obj_align(s_offset_label, LV_ALIGN_TOP_LEFT, 12, 66);

    s_ref_label = lv_label_create(parent);
    lv_obj_add_style(s_ref_label, &s_style_label, LV_PART_MAIN);
    lv_obj_align(s_ref_label, LV_ALIGN_TOP_LEFT, 12, 100);
    update_ref_label();

    lv_obj_t *minus_btn = lv_btn_create(parent);
    lv_obj_set_size(minus_btn, 44, 30);
    lv_obj_align(minus_btn, LV_ALIGN_TOP_LEFT, 12, 124);
    lv_obj_add_event_cb(minus_btn, ref_minus_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_lbl = lv_label_create(minus_btn);
    lv_label_set_text(minus_lbl, "-1");
    lv_obj_center(minus_lbl);

    lv_obj_t *plus_btn = lv_btn_create(parent);
    lv_obj_set_size(plus_btn, 44, 30);
    lv_obj_align(plus_btn, LV_ALIGN_TOP_LEFT, 64, 124);
    lv_obj_add_event_cb(plus_btn, ref_plus_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_label_create(plus_btn);
    lv_label_set_text(plus_lbl, "+1");
    lv_obj_center(plus_lbl);

    lv_obj_t *apply_btn = lv_btn_create(parent);
    lv_obj_set_size(apply_btn, content_w - 24, 30);
    lv_obj_align(apply_btn, LV_ALIGN_TOP_LEFT, 12, 164);
    lv_obj_add_event_cb(apply_btn, apply_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, "Apply: set live reading = Reference");
    lv_obj_center(apply_lbl);

    lv_obj_t *reset_btn = lv_btn_create(parent);
    lv_obj_set_size(reset_btn, content_w - 24, 26);
    lv_obj_align(reset_btn, LV_ALIGN_TOP_LEFT, 12, 200);
    lv_obj_add_event_cb(reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, "Reset offset to 0");
    lv_obj_center(reset_lbl);

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "Sensor calibration screen shown");
}

void sensor_calibration_hide(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    s_reading_label = NULL;
    s_offset_label = NULL;
    s_ref_label = NULL;
}

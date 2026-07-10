/**
 * @file manual_control.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "ui_display/screens/manual_control.h"

static const char *TAG = "manual_control";

static lv_obj_t *s_status_label;
static lv_obj_t *s_fan_label;
static lv_obj_t *s_heater_label;
static lv_obj_t *s_fan_slider;
static lv_obj_t *s_heater_slider;
static lv_timer_t *s_refresh_timer;

static lv_style_t s_style_title;
static lv_style_t s_style_label;
static lv_style_t s_style_value;
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

    lv_style_init(&s_style_value);
    lv_style_set_text_color(&s_style_value, lv_color_hex(0xe0e0e0));

    s_styles_ready = true;
}

static const char *phase_text(roast_phase_t phase)
{
    switch (phase) {
    case ROAST_PHASE_IDLE: return "IDLE";
    case ROAST_PHASE_PREHEAT: return "PREHEAT";
    case ROAST_PHASE_ROASTING: return "ROASTING";
    case ROAST_PHASE_DEVELOPMENT: return "DEVELOPMENT";
    case ROAST_PHASE_COOLING: return "COOLING";
    case ROAST_PHASE_COMPLETED: return "COMPLETED";
    case ROAST_PHASE_ABORTED: return "ABORTED";
    default: return "?";
    }
}

/* Applied on release (not while dragging), so touch drag doesn't fight with
 * the periodic telemetry refresh. Still routed through command_dispatcher,
 * which enforces the same Safety Manager rules (fan floor, heater-requires-
 * fan, alarm-ack gate) as every other command source. */
static void fan_slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    esp_err_t err = command_dispatcher_set_fan_pct((uint8_t)val, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Fan %d%% rejected: %s", (int)val, esp_err_to_name(err));
    }
}

static void heater_slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    esp_err_t err = command_dispatcher_set_heater_pct((uint8_t)val, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Heater %d%% rejected: %s", (int)val, esp_err_to_name(err));
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    char buf[64];
    if (snap.sensor_valid) {
        snprintf(buf, sizeof(buf), "%s - BT: %.1f C", phase_text(snap.phase), snap.bean_temp_c);
    } else {
        snprintf(buf, sizeof(buf), "%s - BT: --", phase_text(snap.phase));
    }
    lv_label_set_text(s_status_label, buf);

    snprintf(buf, sizeof(buf), "Fan: %d%%", snap.fan_pct);
    lv_label_set_text(s_fan_label, buf);
    if (!lv_obj_has_state(s_fan_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_fan_slider, snap.fan_pct, LV_ANIM_OFF);
    }

    snprintf(buf, sizeof(buf), "Heater: %d%%", snap.heater_pct);
    lv_label_set_text(s_heater_label, buf);
    if (!lv_obj_has_state(s_heater_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_heater_slider, snap.heater_pct, LV_ANIM_OFF);
    }
}

void manual_control_show_in(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Manual / Artisan Control");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    s_status_label = lv_label_create(parent);
    lv_obj_add_style(s_status_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_status_label, "IDLE - BT: --");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 40);

    lv_coord_t slider_w = content_w - 24;

    lv_obj_t *fan_hdr = lv_label_create(parent);
    lv_obj_add_style(fan_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(fan_hdr, "Fan");
    lv_obj_align(fan_hdr, LV_ALIGN_TOP_LEFT, 12, 78);

    s_fan_label = lv_label_create(parent);
    lv_obj_add_style(s_fan_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_fan_label, "Fan: 0%");
    lv_obj_align(s_fan_label, LV_ALIGN_TOP_RIGHT, -12, 78);

    s_fan_slider = lv_slider_create(parent);
    lv_obj_set_size(s_fan_slider, slider_w, 20);
    lv_obj_align(s_fan_slider, LV_ALIGN_TOP_LEFT, 12, 104);
    lv_slider_set_range(s_fan_slider, 0, 100);
    lv_obj_add_event_cb(s_fan_slider, fan_slider_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *heater_hdr = lv_label_create(parent);
    lv_obj_add_style(heater_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(heater_hdr, "Heater");
    lv_obj_align(heater_hdr, LV_ALIGN_TOP_LEFT, 12, 150);

    s_heater_label = lv_label_create(parent);
    lv_obj_add_style(s_heater_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_heater_label, "Heater: 0%");
    lv_obj_align(s_heater_label, LV_ALIGN_TOP_RIGHT, -12, 150);

    s_heater_slider = lv_slider_create(parent);
    lv_obj_set_size(s_heater_slider, slider_w, 20);
    lv_obj_align(s_heater_slider, LV_ALIGN_TOP_LEFT, 12, 176);
    lv_slider_set_range(s_heater_slider, 0, 100);
    lv_obj_add_event_cb(s_heater_slider, heater_slider_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, slider_w);
    lv_label_set_text(note, "Fan floor of 30% is enforced automatically while the heater is on (FR-004).");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 12, 212);

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "Manual control screen shown");
}

void manual_control_hide(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
}

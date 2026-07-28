/**
 * @file heater_settings.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "safety/safety_manager.h"
#include "ui_display/screens/settings_hub.h"
#include "ui_display/screens/heater_settings.h"

static const char *TAG = "heater_settings";

static lv_obj_t *s_value_label;
static lv_obj_t *s_slider;

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

static void update_value_label(int32_t pct)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Max Heater Power: %d%%", (int)pct);
    lv_label_set_text(s_value_label, buf);
}

/* Applied on release (not while dragging), same pattern as manual_control.c's
 * sliders - avoids fighting with any periodic refresh. */
static void slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    esp_err_t err = safety_manager_set_max_heater_power_pct((uint8_t)val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist Max Heater Power: %s", esp_err_to_name(err));
    }
    update_value_label(val);
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    heater_settings_hide();
    lv_obj_clean(parent);
    settings_hub_return_to_menu(parent);
}

void heater_settings_show_in(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Max Heater Power");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, parent);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, content_w - 24);
    lv_label_set_text(note,
                       "Caps the heater's PWM duty cycle regardless of what the automatic PID/profile "
                       "control asks for - useful if your resistive element is too strong at full power.");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 12, 44);

    int32_t current_pct = (int32_t)safety_manager_get_max_heater_power_pct();

    s_value_label = lv_label_create(parent);
    lv_obj_add_style(s_value_label, &s_style_label, LV_PART_MAIN);
    update_value_label(current_pct);
    lv_obj_align(s_value_label, LV_ALIGN_TOP_LEFT, 12, 92);

    lv_coord_t slider_margin = 24;
    lv_coord_t slider_w = content_w - (slider_margin * 2);

    s_slider = lv_slider_create(parent);
    lv_obj_set_size(s_slider, slider_w, 20);
    lv_obj_align(s_slider, LV_ALIGN_TOP_LEFT, slider_margin, 118);
    lv_slider_set_range(s_slider, 0, 100);
    lv_slider_set_value(s_slider, current_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_RELEASED, NULL);

    ESP_LOGI(TAG, "Heater settings screen shown (current cap=%d%%)", (int)current_pct);
}

void heater_settings_hide(void)
{
    /* No periodic timer/state to tear down - nothing to do. */
}

/**
 * @file status_screen.c
 * @brief Minimal bring-up/status screen implementation (see header).
 */
#include <stdio.h>
#include <inttypes.h>
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include "hal/max6675.h"
#include "hal/wifi_provisioning.h"
#include "ui_display/status_screen.h"

static const char *TAG = "status_screen";

static lv_obj_t *s_temp_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_touch_label;
static uint32_t s_touch_count;

/* Explicit Material-Design-dark styles, applied directly to each widget.
 * Not relying solely on lv_theme_default_init() here (see display_panel.c) -
 * that call alone did not visibly restyle the already-created default
 * screen object on this hardware, so styles are set directly for a
 * guaranteed, predictable look. */
static lv_style_t s_style_screen;
static lv_style_t s_style_label;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_label;

static void styles_init(void)
{
    lv_style_init(&s_style_screen);
    lv_style_set_bg_color(&s_style_screen, lv_color_hex(0x121212)); /* Material dark surface */
    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);

    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, lv_color_hex(0xE0E0E0));

    lv_style_init(&s_style_btn);
    lv_style_set_bg_color(&s_style_btn, lv_color_hex(0xFF9746)); /* Primary accent */
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn, 6);

    lv_style_init(&s_style_btn_label);
    lv_style_set_text_color(&s_style_btn_label, lv_color_hex(0xFFFFFF));
}

static const char *wifi_state_text(wifi_prov_state_t state)
{
    switch (state) {
    case WIFI_PROV_STATE_AP_PORTAL: return "AP setup (connect to PopRoaster-Setup)";
    case WIFI_PROV_STATE_CONNECTING: return "Connecting to WiFi...";
    case WIFI_PROV_STATE_CONNECTED: return "WiFi connected";
    case WIFI_PROV_STATE_FAILED: return "WiFi failed - re-entering AP setup";
    default: return "Unknown";
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    max6675_sample_t sample;
    if (max6675_read(&sample) == ESP_OK && sample.quality == MAX6675_QUALITY_VALID) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Bean Temp: %.1f C", sample.bean_temp_c);
        lv_label_set_text(s_temp_label, buf);
    } else {
        lv_label_set_text(s_temp_label, "Bean Temp: sensor invalid");
    }

    lv_label_set_text(s_wifi_label, wifi_state_text(wifi_provisioning_get_state()));
}

static void touch_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_touch_count++;
    char buf[32];
    snprintf(buf, sizeof(buf), "Touch OK (x%" PRIu32 ")", s_touch_count);
    lv_label_set_text(s_touch_label, buf);
}

esp_err_t ui_status_screen_show(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
        return ESP_FAIL;
    }

    styles_init();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_add_style(scr, &s_style_screen, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_add_style(title, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(title, "Pop Roaster");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_temp_label = lv_label_create(scr);
    lv_obj_add_style(s_temp_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_temp_label, "Bean Temp: --");
    lv_obj_align(s_temp_label, LV_ALIGN_TOP_MID, 0, 50);

    s_wifi_label = lv_label_create(scr);
    lv_obj_add_style(s_wifi_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_wifi_label, "WiFi: --");
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_add_style(btn, &s_style_btn, LV_PART_MAIN);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_event_cb(btn, touch_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_obj_add_style(btn_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(btn_label, "Tap to test touch");

    s_touch_label = lv_label_create(scr);
    lv_obj_add_style(s_touch_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_touch_label, "Touch OK (x0)");
    lv_obj_align(s_touch_label, LV_ALIGN_CENTER, 0, 90);

    lv_timer_create(refresh_timer_cb, 1000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Status screen shown");
    return ESP_OK;
}

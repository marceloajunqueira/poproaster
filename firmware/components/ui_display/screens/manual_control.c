/**
 * @file manual_control.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "hal/fan_pwm.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/profile_curve_follower.h"
#include "ui_display/screens/manual_control.h"

static const char *TAG = "manual_control";

static lv_obj_t *s_status_label;
static lv_obj_t *s_fan_label;
static lv_obj_t *s_target_label;
static lv_obj_t *s_fan_slider;
static lv_obj_t *s_target_slider;
static lv_obj_t *s_heater_status_label;
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
 * the periodic sync below. Still routed through command_dispatcher, which
 * enforces the same Safety Manager rules (fan floor, heater-requires-fan,
 * alarm-ack gate) as every other command source.
 *
 * BUG FIX (operator report): the slider used to visually "jump to the
 * finger, jump back, then jump again" on release. Root cause: this
 * screen's OWN sync (below) read the fan value back from
 * roast_telemetry_service's CACHED snapshot, which only refreshes on its
 * own independent 500ms timer - for up to that long after release, it
 * still reflected the OLD value, snapping the slider back until the cache
 * caught up. Fixed by reading straight from the HAL (fan_pwm_get_pct()),
 * which is always synchronously up to date (fan_pwm_set_pct() updates it
 * immediately, no polling lag). */
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

/* Per operator requirement: the heater is never a direct setpoint, even in
 * Manual mode - the operator only picks a target BEAN TEMPERATURE, and
 * profile_curve_follower.c's closed-loop PID drives the actual heater duty
 * automatically toward it (same controller Profile mode uses), auto-
 * raising the fan to the 30% floor first if it was left off. */
static void target_slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    profile_curve_follower_set_manual_target_temp_c((float)val);
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

    /* Fan syncs from the HAL directly (see fan_slider_event_cb() comment
     * above) - always immediately accurate, no cache lag. */
    uint8_t fan_pct = fan_pwm_get_pct();
    snprintf(buf, sizeof(buf), "Fan: %d%%", fan_pct);
    lv_label_set_text(s_fan_label, buf);
    if (!lv_obj_has_state(s_fan_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_fan_slider, fan_pct, LV_ANIM_OFF);
    }

    /* Target Temp syncs from profile_curve_follower's own module state
     * (also always immediately accurate - a plain variable, not a polled
     * cache), so it reflects the real current target even after
     * navigating away and back. */
    float target_c = profile_curve_follower_get_manual_target_temp_c();
    snprintf(buf, sizeof(buf), "Target: %.0f C", (double)target_c);
    lv_label_set_text(s_target_label, buf);
    if (!lv_obj_has_state(s_target_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_target_slider, (int32_t)target_c, LV_ANIM_OFF);
    }

    /* Read-only readout of how the automatic thermal-stabilization
     * algorithm is actually responding - operator request, exact format
     * "Temp: 0.0 C / Heater: 100%". */
    if (snap.sensor_valid) {
        snprintf(buf, sizeof(buf), "Temp: %.1f C / Heater: %d%%", snap.bean_temp_c, snap.heater_pct);
    } else {
        snprintf(buf, sizeof(buf), "Temp: -- / Heater: %d%%", snap.heater_pct);
    }
    lv_label_set_text(s_heater_status_label, buf);
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

    /* Operator report: the slider's round knob overflowed past the screen
     * edges - it extends beyond the track's own bounding box by its own
     * radius, so a slider needs a bigger side margin than a plain label/
     * button row. Bumped from 12px to 24px on each side. */
    lv_coord_t slider_margin = 24;
    lv_coord_t slider_w = content_w - (slider_margin * 2);

    lv_obj_t *fan_hdr = lv_label_create(parent);
    lv_obj_add_style(fan_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(fan_hdr, "Fan");
    lv_obj_align(fan_hdr, LV_ALIGN_TOP_LEFT, slider_margin, 70);

    s_fan_label = lv_label_create(parent);
    lv_obj_add_style(s_fan_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_fan_label, "Fan: 0%");
    lv_obj_align(s_fan_label, LV_ALIGN_TOP_RIGHT, -slider_margin, 70);

    s_fan_slider = lv_slider_create(parent);
    lv_obj_set_size(s_fan_slider, slider_w, 20);
    lv_obj_align(s_fan_slider, LV_ALIGN_TOP_LEFT, slider_margin, 94);
    lv_slider_set_range(s_fan_slider, 0, 100);
    lv_obj_add_event_cb(s_fan_slider, fan_slider_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *target_hdr = lv_label_create(parent);
    lv_obj_add_style(target_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(target_hdr, "Target Temp");
    lv_obj_align(target_hdr, LV_ALIGN_TOP_LEFT, slider_margin, 132);

    s_target_label = lv_label_create(parent);
    lv_obj_add_style(s_target_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_target_label, "Target: 0 C");
    lv_obj_align(s_target_label, LV_ALIGN_TOP_RIGHT, -slider_margin, 132);

    s_target_slider = lv_slider_create(parent);
    lv_obj_set_size(s_target_slider, slider_w, 20);
    lv_obj_align(s_target_slider, LV_ALIGN_TOP_LEFT, slider_margin, 156);
    lv_slider_set_range(s_target_slider, 0, 260);
    lv_obj_add_event_cb(s_target_slider, target_slider_event_cb, LV_EVENT_RELEASED, NULL);

    s_heater_status_label = lv_label_create(parent);
    lv_obj_add_style(s_heater_status_label, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(s_heater_status_label, "Temp: -- / Heater: 0%");
    lv_obj_align(s_heater_status_label, LV_ALIGN_TOP_LEFT, slider_margin, 188);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, slider_w);
    lv_label_set_text(note, "Heater is automatic (PID to Target Temp); fan auto-raises to the 30% floor if needed.");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, slider_margin, 212);

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

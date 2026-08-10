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
#include "roast_core/session_state_machine.h"
#include "safety/safety_manager.h"
#include "ui_display/screens/manual_control.h"

static const char *TAG = "manual_control";

#define FAN_BAR_COUNT 3

static lv_obj_t *s_status_label;
static lv_obj_t *s_fan_label;
static lv_obj_t *s_fan_bars[FAN_BAR_COUNT];
static lv_obj_t *s_target_label;
static lv_obj_t *s_target_slider;
static lv_obj_t *s_heater_status_label;
static lv_obj_t *s_control_note_label;
static lv_timer_t *s_refresh_timer;

static uint8_t s_fan_level = 0;
static bool s_target_dirty = false;
static int32_t s_target_pending = 0;

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

/* Per operator request: the fan only has 5 usable discrete speeds (the
 * motor doesn't behave predictably at arbitrary percentages) - 0=off,
 * 1..3 map to hal/fan_pwm.h's fixed level table (80/90/100%). Applied
 * immediately (no separate Apply step needed for discrete +/- taps, unlike
 * the Target Temp slider below). Still routed through command_dispatcher,
 * which enforces the same Safety Manager rules (fan floor, heater-requires-
 * fan, alarm-ack gate) as every other command source. */
static void apply_fan_level(uint8_t level)
{
    if (level > FAN_PWM_LEVEL_MAX) {
        level = FAN_PWM_LEVEL_MAX;
    }
    s_fan_level = level;
    uint8_t pct = fan_pwm_level_to_pct(level);
    esp_err_t err = command_dispatcher_set_fan_pct(pct, SAFETY_CMD_SOURCE_DISPLAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Fan level %d (%d%%) rejected: %s", (int)level, (int)pct, esp_err_to_name(err));
    }
}

static void fan_minus_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_fan_level > 0) {
        apply_fan_level(s_fan_level - 1);
    }
}

static void fan_plus_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_fan_level < FAN_PWM_LEVEL_MAX) {
        apply_fan_level(s_fan_level + 1);
    }
}

/* Per operator request: an explicit "Parar" (Stop) button that always
 * drops the fan straight to level 0, regardless of the current level -
 * faster/more certain than tapping "-" repeatedly. */
static void fan_stop_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    apply_fan_level(0);
}

/* Tapping a level box jumps straight to it, instead of only being able to
 * step one level at a time via +/-. */
static void fan_bar_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uint8_t level = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    apply_fan_level(level);
}

/* Per operator request: the Target Temp slider no longer applies on
 * release directly - it only stages a PENDING value (guards against
 * accidentally starting the heater toward the wrong temperature from a
 * slip of the finger). The new "Aplicar" button commits it. Bound to both
 * VALUE_CHANGED (fires continuously while dragging, for live feedback -
 * operator report: with only RELEASED, the label never moved while
 * dragging so you had to guess the position) and RELEASED (covers the
 * final position once the finger lifts). */
static void target_slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }
    s_target_pending = lv_slider_get_value(lv_event_get_target(e));
    s_target_dirty = true;
    char buf[48];
    snprintf(buf, sizeof(buf), "Pending: %d C (tap Aplicar)", (int)s_target_pending);
    lv_label_set_text(s_target_label, buf);
}

static void target_apply_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    profile_curve_follower_set_manual_target_temp_c((float)s_target_pending);
    s_target_dirty = false;
}

/* Per operator request: an explicit "Desligar" (turn off) button that
 * immediately zeroes the Target Temp - no staging/Aplicar needed, since
 * this is meant to interrupt heating right away (heater_pid_update(0, ...)
 * with any BT above a few degrees hits the PID's hard overshoot cutoff on
 * the very next tick, forcing the heater fully off). Doesn't touch the
 * slider position - 0 is below MANUAL_TARGET_TEMP_MIN_C (it's just the
 * "off" sentinel, not a real operating point on the dial) so the slider
 * simply stays wherever it was for whenever the operator turns back on. */
static void target_off_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_target_pending = 0;
    s_target_dirty = false;
    profile_curve_follower_set_manual_target_temp_c(0.0f);
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);

    char buf[96];
    if (snap.sensor_valid) {
        snprintf(buf, sizeof(buf), "%s - BT: %.1f C", phase_text(snap.phase), snap.bean_temp_c);
    } else {
        snprintf(buf, sizeof(buf), "%s - BT: --", phase_text(snap.phase));
    }
    lv_label_set_text(s_status_label, buf);

    /* Fan level syncs from the HAL directly (fan_pwm_set_pct() updates its
     * cached value synchronously, no polling lag), converted back to the
     * nearest discrete level for display. */
    uint8_t fan_pct = fan_pwm_get_pct();
    s_fan_level = fan_pwm_pct_to_level(fan_pct);
    snprintf(buf, sizeof(buf), "Level %d (%d%%)", (int)s_fan_level, (int)fan_pct);
    lv_label_set_text(s_fan_label, buf);
    for (int i = 0; i < FAN_BAR_COUNT; i++) {
        bool lit = (uint8_t)(i + 1) <= s_fan_level;
        lv_obj_set_style_bg_color(s_fan_bars[i], lit ? lv_color_hex(0xFF9746) : lv_color_hex(0x333333), LV_PART_MAIN);
    }

    /* Target Temp: while a pending (unapplied) change exists, don't let the
     * periodic sync below overwrite it - only resync from the actually-
     * applied value once "Aplicar" clears the pending flag. Also skip the
     * resync while target_c is the "off" sentinel (0, below
     * MANUAL_TARGET_TEMP_MIN_C) - the slider's range no longer reaches
     * down there, so leave it showing wherever it last was. */
    float target_c = profile_curve_follower_get_manual_target_temp_c();
    if (s_target_dirty) {
        snprintf(buf, sizeof(buf), "Pending: %d C (tap Aplicar)", (int)s_target_pending);
    } else {
        snprintf(buf, sizeof(buf), "Target: %.0f C", (double)target_c);
        if (!lv_obj_has_state(s_target_slider, LV_STATE_PRESSED) && target_c >= MANUAL_TARGET_TEMP_MIN_C) {
            lv_slider_set_value(s_target_slider, (int32_t)target_c, LV_ANIM_OFF);
        }
    }
    lv_label_set_text(s_target_label, buf);

    /* Read-only readout of how the automatic thermal-stabilization
     * algorithm is actually responding - operator request, exact format
     * "Temp: 0.0 C / Heater: 100%". Also shows the PID's own logical
     * request whenever Max Heater Power is actively scaling it down
     * (otherwise looks identical to a plain ceiling - see
     * safety_manager_get_last_requested_heater_pct()'s doc comment). */
    uint8_t requested_heater_pct = safety_manager_get_last_requested_heater_pct();
    bool heater_capped = (requested_heater_pct != (uint8_t)snap.heater_pct);
    if (snap.sensor_valid) {
        if (heater_capped) {
            snprintf(buf, sizeof(buf), "Temp: %.1f C / Heater: %d%% (wants %d%%, capped)",
                     snap.bean_temp_c, snap.heater_pct, (int)requested_heater_pct);
        } else {
            snprintf(buf, sizeof(buf), "Temp: %.1f C / Heater: %d%%", snap.bean_temp_c, snap.heater_pct);
        }
    } else {
        if (heater_capped) {
            snprintf(buf, sizeof(buf), "Temp: -- / Heater: %d%% (wants %d%%, capped)",
                     snap.heater_pct, (int)requested_heater_pct);
        } else {
            snprintf(buf, sizeof(buf), "Temp: -- / Heater: %d%%", snap.heater_pct);
        }
    }
    lv_label_set_text(s_heater_status_label, buf);

    /* Operator requirement: Manual control must be fully live regardless of
     * whether a formal roast session was ever started (the operator may
     * just want direct control, or to hand off to Artisan, without going
     * through "Start Roast" at all) - profile_curve_follower.c now drives
     * the Target Temp PID unconditionally whenever no Profile-mode session
     * is actively in control. The only remaining reasons the heater might
     * not respond are a Profile preset actively driving things, Cooling,
     * or a paused session - surface those, otherwise just show the normal
     * automatic-control note. */
    const roast_session_t *session = session_sm_get_state();
    bool profile_active = (session->control_mode == ROAST_MODE_PROFILE) &&
                           (session->phase == ROAST_PHASE_PREHEAT || session->phase == ROAST_PHASE_ROASTING ||
                            session->phase == ROAST_PHASE_DEVELOPMENT);
    if (session->phase == ROAST_PHASE_COOLING) {
        lv_label_set_text(s_control_note_label, "Cooling - heater stays off regardless of Target Temp.");
    } else if (profile_active) {
        lv_label_set_text(s_control_note_label,
                           "A Profile preset is controlling the heater right now - Target Temp below is ignored. "
                           "Cancel the session and pick no preset (Presets tab) to use Manual mode instead.");
    } else if (session->paused) {
        lv_label_set_text(s_control_note_label, "Session paused - heater control is frozen until resumed.");
    } else {
        lv_label_set_text(s_control_note_label,
                           "Heater is automatic (PID to Target Temp); fan auto-raises to Level 1 (90%) if needed.");
    }
}

void manual_control_show_in(lv_obj_t *parent)
{
    ensure_styles();
    s_target_dirty = false;

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Manual / Artisan Control");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

    s_status_label = lv_label_create(parent);
    lv_obj_add_style(s_status_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_status_label, "IDLE - BT: --");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 26);

    lv_coord_t margin = 20;
    lv_coord_t usable_w = content_w - (margin * 2);

    lv_obj_t *fan_hdr = lv_label_create(parent);
    lv_obj_add_style(fan_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(fan_hdr, "Fan");
    lv_obj_align(fan_hdr, LV_ALIGN_TOP_LEFT, margin, 50);

    s_fan_label = lv_label_create(parent);
    lv_obj_add_style(s_fan_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_fan_label, "Level 0 (0%)");
    lv_obj_align(s_fan_label, LV_ALIGN_TOP_RIGHT, -margin, 50);

    /* 3 boxes, lit up to the current level (like a signal-strength meter) -
     * lets the operator see fan speed at a glance without doing % math.
     * Also directly clickable to jump to that level. */
    lv_coord_t bar_gap = 6;
    lv_coord_t bar_h = 22;
    lv_coord_t bar_w = (usable_w - bar_gap * (FAN_BAR_COUNT - 1)) / FAN_BAR_COUNT;
    for (int i = 0; i < FAN_BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, bar_w, bar_h);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, margin + i * (bar_w + bar_gap), 72);
        lv_obj_add_event_cb(bar, fan_bar_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        s_fan_bars[i] = bar;
    }

    lv_coord_t fan_btn_y = 72 + bar_h + 4;
    lv_obj_t *fan_minus_btn = lv_btn_create(parent);
    lv_obj_set_size(fan_minus_btn, 48, 28);
    lv_obj_align(fan_minus_btn, LV_ALIGN_TOP_LEFT, margin, fan_btn_y);
    lv_obj_add_event_cb(fan_minus_btn, fan_minus_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fan_minus_lbl = lv_label_create(fan_minus_btn);
    lv_label_set_text(fan_minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(fan_minus_lbl);

    lv_obj_t *fan_plus_btn = lv_btn_create(parent);
    lv_obj_set_size(fan_plus_btn, 48, 28);
    lv_obj_align(fan_plus_btn, LV_ALIGN_TOP_RIGHT, -margin, fan_btn_y);
    lv_obj_add_event_cb(fan_plus_btn, fan_plus_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fan_plus_lbl = lv_label_create(fan_plus_btn);
    lv_label_set_text(fan_plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(fan_plus_lbl);

    lv_obj_t *fan_stop_btn = lv_btn_create(parent);
    lv_obj_set_size(fan_stop_btn, 90, 28);
    lv_obj_align(fan_stop_btn, LV_ALIGN_TOP_MID, 0, fan_btn_y);
    lv_obj_set_style_bg_color(fan_stop_btn, lv_color_hex(0xB3261E), LV_PART_MAIN);
    lv_obj_add_event_cb(fan_stop_btn, fan_stop_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fan_stop_lbl = lv_label_create(fan_stop_btn);
    lv_label_set_text(fan_stop_lbl, LV_SYMBOL_STOP " Parar");
    lv_obj_center(fan_stop_lbl);

    lv_obj_t *target_hdr = lv_label_create(parent);
    lv_obj_add_style(target_hdr, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(target_hdr, "Target Temp");
    lv_obj_align(target_hdr, LV_ALIGN_TOP_LEFT, margin, 134);

    s_target_label = lv_label_create(parent);
    lv_obj_add_style(s_target_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_target_label, "Target: 0 C");
    lv_obj_align(s_target_label, LV_ALIGN_TOP_RIGHT, -margin, 134);

    s_target_slider = lv_slider_create(parent);
    lv_obj_set_size(s_target_slider, usable_w, 20);
    lv_obj_align(s_target_slider, LV_ALIGN_TOP_LEFT, margin, 156);
    /* Range is MANUAL_TARGET_TEMP_MIN_C..MAX_C, not 0..260 - operator
     * testing showed the plastic housing melts above 220C, and there's no
     * reason to dial below 120C for actual roasting (0/off is a separate
     * sentinel, reached via "Desligar" below, not a dial position). */
    lv_slider_set_range(s_target_slider, (int32_t)MANUAL_TARGET_TEMP_MIN_C, (int32_t)MANUAL_TARGET_TEMP_MAX_C);
    lv_obj_add_event_cb(s_target_slider, target_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_target_slider, target_slider_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *target_apply_btn = lv_btn_create(parent);
    lv_obj_set_size(target_apply_btn, 90, 26);
    lv_obj_align(target_apply_btn, LV_ALIGN_TOP_RIGHT, -margin, 182);
    lv_obj_add_event_cb(target_apply_btn, target_apply_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *target_apply_lbl = lv_label_create(target_apply_btn);
    lv_label_set_text(target_apply_lbl, LV_SYMBOL_OK " Aplicar");
    lv_obj_center(target_apply_lbl);

    lv_obj_t *target_off_btn = lv_btn_create(parent);
    lv_obj_set_size(target_off_btn, 90, 26);
    lv_obj_align(target_off_btn, LV_ALIGN_TOP_LEFT, margin, 182);
    lv_obj_set_style_bg_color(target_off_btn, lv_color_hex(0xB3261E), LV_PART_MAIN);
    lv_obj_add_event_cb(target_off_btn, target_off_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *target_off_lbl = lv_label_create(target_off_btn);
    lv_label_set_text(target_off_lbl, LV_SYMBOL_STOP " Desligar");
    lv_obj_center(target_off_lbl);

    s_heater_status_label = lv_label_create(parent);
    lv_obj_add_style(s_heater_status_label, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(s_heater_status_label, "Temp: -- / Heater: 0%");
    lv_obj_align(s_heater_status_label, LV_ALIGN_TOP_LEFT, margin, 212);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, usable_w);
    lv_label_set_text(note, "Heater is automatic (PID to Target Temp); fan auto-raises to Level 1 (90%) if needed.");
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, margin, 232);
    s_control_note_label = note;

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

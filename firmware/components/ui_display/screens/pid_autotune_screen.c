/**
 * @file pid_autotune_screen.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "hal/fan_pwm.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/pid_autotune.h"
#include "roast_core/roast_telemetry_service.h"
#include "safety/safety_manager.h"
#include "ui_display/i18n.h"
#include "ui_display/screens/settings_hub.h"
#include "ui_display/screens/pid_autotune_screen.h"

static const char *TAG = "pid_autotune_screen";

/* Fully automatic per operator request ("decida os melhores parametros") -
 * no fields to fill in, just Start. Evidence behind these numbers:
 * - Fan forced to FAN_PWM_LEVEL_MAX (100%): airflow dominates this plant's
 *   gain (measured: the same heater duty settles ~30C higher at a lower fan
 *   level), so full airflow is both the most repeatable condition AND the
 *   one with the most headroom before the thermal protector/limits trip.
 * - Setpoint 130C: a logged open-loop step test at 64% duty/100% fan
 *   plateaued around 140C without ever tripping anything - well clear of
 *   the 230C warning / 240C absolute cutoff.
 * - Relay swing 70/0%: strong enough for a clean, fast-converging
 *   oscillation (d=35) without being an extreme duty cycle. */
#define AUTOTUNE_SETPOINT_C 130.0f
#define AUTOTUNE_RELAY_HIGH_PCT 70
#define AUTOTUNE_RELAY_LOW_PCT 0
/* Fan soft-start ramp is ~2000ms (hal/fan_pwm.c) - wait a bit longer than
 * that before actually starting the relay, so pid_autotune_start()'s own
 * fan-floor check sees the real, settled speed, not mid-ramp. */
#define AUTOTUNE_FAN_RAMP_MS 2500
/* Shared with show_in()'s creation-time layout and refresh_timer_cb()'s
 * per-tick repositioning below - must match. */
#define AUTOTUNE_UI_MARGIN 12

static lv_obj_t *s_consent_cb;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_live_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_result_label;
static lv_timer_t *s_refresh_timer;

static bool s_pending_start;
static int64_t s_pending_start_at_ms;

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

static void update_start_btn_enabled(void)
{
    pid_autotune_status_t st;
    pid_autotune_get_status(&st);
    bool active = (st.state == PID_AUTOTUNE_RUNNING) || s_pending_start;
    bool consented = lv_obj_has_state(s_consent_cb, LV_STATE_CHECKED);
    if (consented && !active) {
        lv_obj_clear_state(s_start_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
    }
}

static void consent_cb_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    update_start_btn_enabled();
}

/* Raises the fan to full speed first (autotune's own fan-floor check needs
 * the REAL, settled speed - see AUTOTUNE_FAN_RAMP_MS above), then starts the
 * relay run once the ramp has had time to finish. */
static void start_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!lv_obj_has_state(s_consent_cb, LV_STATE_CHECKED) || pid_autotune_is_active()) {
        return;
    }
    command_dispatcher_set_fan_pct(fan_pwm_level_to_pct(FAN_PWM_LEVEL_MAX), SAFETY_CMD_SOURCE_DISPLAY);
    s_pending_start = true;
    s_pending_start_at_ms = (esp_timer_get_time() / 1000) + AUTOTUNE_FAN_RAMP_MS;
    lv_label_set_text(s_status_label, i18n_get(I18N_KEY_PID_AUTOTUNE_STATUS_PREPARING));
    update_start_btn_enabled();
}

static void abort_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_pending_start = false;

    /* ORDER MATTERS (bug report: "cancelei e o fan continuou ligado" even
     * with BT barely above room temp): safety_manager_request_fan_pct()
     * rejects ANY fan request below the 80% heating floor - including 0%
     * (off) - while ssr_heater_get_duty_pct() is still > 0. The heater MUST
     * be commanded off first so that check is already satisfied by the
     * time the fan-off request is evaluated; doing it in the other order
     * (as before) meant the fan-off almost always lost this race against
     * whatever duty the relay had last set, regardless of BT. */
    command_dispatcher_set_heater_pct(0, SAFETY_CMD_SOURCE_DISPLAY);

    /* Raising the fan to full speed to start the run was OUR side effect
     * (start_btn_event_cb above) - Cancel must actually try to hand it back,
     * not just leave it spinning. This can still be rejected (fan stays on,
     * same as everywhere else in the firmware) while BT is still >= 100C -
     * say so plainly in the status message instead of leaving the operator
     * to guess why the fan kept running. Emergency Stop is the only path
     * that bypasses that floor. */
    esp_err_t fan_err = command_dispatcher_set_fan_pct(0, SAFETY_CMD_SOURCE_DISPLAY);
    pid_autotune_abort(fan_err == ESP_OK ? "Cancelled by operator - fan off"
                                          : "Cancelled by operator - fan stays on until BT < 100C (safety)");
    update_start_btn_enabled();
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    pid_autotune_screen_hide();
    lv_obj_clean(parent);
    settings_hub_return_to_menu(parent);
}

static const char *state_str(pid_autotune_state_t state)
{
    switch (state) {
    case PID_AUTOTUNE_RUNNING: return i18n_get(I18N_KEY_PID_AUTOTUNE_STATE_RUNNING);
    case PID_AUTOTUNE_SUCCEEDED: return i18n_get(I18N_KEY_PID_AUTOTUNE_STATE_SUCCEEDED);
    case PID_AUTOTUNE_FAILED: return i18n_get(I18N_KEY_PID_AUTOTUNE_STATE_FAILED);
    default: return i18n_get(I18N_KEY_PID_AUTOTUNE_STATE_IDLE);
    }
}

/* Positions `obj` right below `above` (plus `gap`) instead of a hand-picked
 * fixed y - the note/status/result labels wrap to a different number of
 * lines per language, so a fixed pixel budget overlapped the next widget in
 * some cases (bug report: checkbox landing on top of the warning text). */
static lv_coord_t stack_below(lv_obj_t *parent, lv_obj_t *obj, lv_obj_t *above, lv_coord_t x, lv_coord_t gap)
{
    lv_obj_update_layout(parent);
    lv_coord_t y = lv_obj_get_y(above) + lv_obj_get_height(above) + gap;
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    return y;
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_pending_start && (esp_timer_get_time() / 1000) >= s_pending_start_at_ms) {
        s_pending_start = false;
        esp_err_t err = pid_autotune_start(AUTOTUNE_SETPOINT_C, AUTOTUNE_RELAY_HIGH_PCT, AUTOTUNE_RELAY_LOW_PCT);
        if (err != ESP_OK) {
            char buf[96];
            snprintf(buf, sizeof(buf), i18n_get(I18N_KEY_PID_AUTOTUNE_START_ERROR_FMT), esp_err_to_name(err));
            lv_label_set_text(s_status_label, buf);
            update_start_btn_enabled();
            return;
        }
    }

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);
    char live_buf[64];
    if (snap.sensor_valid) {
        snprintf(live_buf, sizeof(live_buf), i18n_get(I18N_KEY_PID_AUTOTUNE_LIVE_FMT), snap.fan_pct,
                 (double)snap.bean_temp_c, snap.heater_pct);
    } else {
        snprintf(live_buf, sizeof(live_buf), i18n_get(I18N_KEY_PID_AUTOTUNE_LIVE_FMT_NO_BT), snap.fan_pct,
                 snap.heater_pct);
    }
    lv_label_set_text(s_live_label, live_buf);

    pid_autotune_status_t st;
    pid_autotune_get_status(&st);

    char buf[160];
    if (s_pending_start) {
        snprintf(buf, sizeof(buf), "%s", i18n_get(I18N_KEY_PID_AUTOTUNE_STATUS_PREPARING));
    } else if (st.state == PID_AUTOTUNE_IDLE) {
        snprintf(buf, sizeof(buf), "%s", i18n_get(I18N_KEY_PID_AUTOTUNE_STATUS_IDLE));
    } else {
        snprintf(buf, sizeof(buf), i18n_get(I18N_KEY_PID_AUTOTUNE_STATUS_FMT), state_str(st.state),
                 (unsigned)st.elapsed_s, (unsigned)st.phase_count, st.message);
    }
    lv_label_set_text(s_status_label, buf);

    if (st.state == PID_AUTOTUNE_SUCCEEDED) {
        /* Auto-applied+saved by profile_curve_follower.c's drive_autotune()
         * the instant the run converges (works even if nobody is looking
         * at this screen) - this just reports the result. */
        snprintf(buf, sizeof(buf), i18n_get(I18N_KEY_PID_AUTOTUNE_RESULT_APPLIED_FMT), (double)st.no_overshoot.kp,
                 (double)st.no_overshoot.ki, (double)st.no_overshoot.kd);
        lv_label_set_text(s_result_label, buf);
    } else if (st.state == PID_AUTOTUNE_FAILED) {
        lv_label_set_text(s_result_label, i18n_get(I18N_KEY_PID_AUTOTUNE_RESULT_FAILED));
    } else {
        lv_label_set_text(s_result_label, "");
    }

    /* The status line's height varies a lot (short "Idle..." vs a long
     * technical failure message) - reposition the result label below it on
     * EVERY tick, not just at screen creation, or a longer status line ends
     * up drawn underneath (bug report: "Cancelled by operator" wrapped and
     * landed on top of "No gains were applied"). */
    stack_below(lv_obj_get_parent(s_status_label), s_result_label, s_status_label, AUTOTUNE_UI_MARGIN, 6);

    update_start_btn_enabled();
}

void pid_autotune_screen_show_in(lv_obj_t *parent)
{
    ensure_styles();
    s_pending_start = false;

    const lv_coord_t content_w = lv_obj_get_width(parent);
    const lv_coord_t margin = AUTOTUNE_UI_MARGIN;

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, i18n_get(I18N_KEY_PID_AUTOTUNE));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, margin, 6);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -margin, 4);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, parent);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, i18n_get(I18N_KEY_BACK));
    lv_obj_center(back_lbl);

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_add_style(note, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, content_w - 2 * margin);
    lv_label_set_text(note, i18n_get(I18N_KEY_PID_AUTOTUNE_NOTE));
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, margin, 32);

    s_consent_cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(s_consent_cb, i18n_get(I18N_KEY_PID_AUTOTUNE_CONSENT));
    lv_obj_add_event_cb(s_consent_cb, consent_cb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    stack_below(parent, s_consent_cb, note, margin, 10);

    s_start_btn = lv_btn_create(parent);
    lv_obj_set_size(s_start_btn, 100, 30);
    lv_coord_t btn_row_y = stack_below(parent, s_start_btn, s_consent_cb, margin, 10);
    lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_start_btn, start_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(s_start_btn);
    lv_label_set_text(start_lbl, i18n_get(I18N_KEY_PID_AUTOTUNE_START));
    lv_obj_center(start_lbl);

    lv_obj_t *abort_btn = lv_btn_create(parent);
    lv_obj_set_size(abort_btn, 100, 30);
    lv_obj_align(abort_btn, LV_ALIGN_TOP_LEFT, margin + 110, btn_row_y);
    lv_obj_set_style_bg_color(abort_btn, lv_color_hex(0xB3261E), LV_PART_MAIN);
    lv_obj_add_event_cb(abort_btn, abort_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *abort_lbl = lv_label_create(abort_btn);
    lv_label_set_text(abort_lbl, i18n_get(I18N_KEY_PID_AUTOTUNE_CANCEL));
    lv_obj_center(abort_lbl);

    s_live_label = lv_label_create(parent);
    lv_obj_add_style(s_live_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_live_label, "");
    stack_below(parent, s_live_label, s_start_btn, margin, 12);

    s_status_label = lv_label_create(parent);
    lv_obj_add_style(s_status_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, content_w - 2 * margin);
    lv_label_set_text(s_status_label, i18n_get(I18N_KEY_PID_AUTOTUNE_STATUS_IDLE));
    stack_below(parent, s_status_label, s_live_label, margin, 10);

    s_result_label = lv_label_create(parent);
    lv_obj_add_style(s_result_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(s_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_result_label, content_w - 2 * margin);
    lv_label_set_text(s_result_label, "");
    stack_below(parent, s_result_label, s_status_label, margin, 6);

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "PID Autotune screen shown");
}

void pid_autotune_screen_hide(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
}


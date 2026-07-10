/**
 * @file roast_dashboard.c
 * @brief Real-time roast telemetry dashboard implementation (see header).
 */
#include <stdio.h>
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include "roast_core/session_state_machine.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/roast_profile.h"
#include "storage/profile_store.h"
#include "storage/session_store.h"
#include "hal/wifi_provisioning.h"
#include "ui_display/screens/roast_dashboard.h"

static const char *TAG = "roast_dashboard";

/* The live chart's timeline spans the exact total duration of the selected
 * preset (sum of every setpoint's duration) - not a fixed window - so the
 * target and actual curves always line up. Falls back to a fixed 20-minute
 * window when no preset is selected. */
#define MAX_CHART_POINTS 150
#define DEFAULT_CHART_DURATION_S (20 * 60)

static lv_obj_t *s_chart;
static lv_chart_series_t *s_chart_bt_series;
static lv_chart_series_t *s_chart_fan_series;
static lv_obj_t *s_target_temp_line;
static lv_obj_t *s_target_fan_line;
static lv_point_t s_target_temp_points[MAX_CHART_POINTS];
static lv_point_t s_target_fan_points[MAX_CHART_POINTS];
static uint32_t s_chart_duration_s = DEFAULT_CHART_DURATION_S;

/* Segment value labels (rebuild_target_lines()) - tracked so they can be
 * deleted before rebuilding, since that function can now run more than
 * once per screen lifetime (sync_selected_profile_if_changed() re-runs it
 * whenever the operator picks a different preset elsewhere while this
 * screen stays open) instead of only once at construction time. */
#define MAX_SEGMENT_LABELS (ROAST_PROFILE_MAX_POINTS * 2)
static lv_obj_t *s_segment_labels[MAX_SEGMENT_LABELS];
static int s_segment_label_count;

static roast_profile_t s_active_profile;
static bool s_has_profile = false;
static int s_last_known_selected_id = -2; /* sentinel distinct from -1 ("no preset selected") so the very first tick always syncs. */

static lv_obj_t *s_preset_label;
static lv_obj_t *s_wifi_ip_label;
static lv_obj_t *s_phase_label;
static lv_obj_t *s_timer_label;
static lv_obj_t *s_bt_label;
static lv_obj_t *s_ror_label;
static lv_obj_t *s_dtr_label;
static lv_obj_t *s_fan_label;
static lv_obj_t *s_heater_label;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_start_btn_label;
static lv_obj_t *s_charge_group;
static lv_obj_t *s_charge_btn;
static lv_obj_t *s_charge_btn_label;
static lv_obj_t *s_charge_cancel_btn;
static lv_obj_t *s_active_group;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_pause_btn_label;
static lv_obj_t *s_cancel_btn;

static lv_timer_t *s_refresh_timer;

static lv_style_t s_style_label;
static lv_style_t s_style_value;
static lv_style_t s_style_btn_primary;
static lv_style_t s_style_btn_secondary;
static lv_style_t s_style_btn_label;
static lv_style_t s_style_seg_temp_label;
static lv_style_t s_style_seg_fan_label;
static bool s_styles_ready = false;

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }

    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, lv_color_hex(0x9e9e9e));

    lv_style_init(&s_style_value);
    lv_style_set_text_color(&s_style_value, lv_color_hex(0xe0e0e0));

    lv_style_init(&s_style_btn_primary);
    lv_style_set_bg_color(&s_style_btn_primary, lv_color_hex(0xFF9746));
    lv_style_set_bg_opa(&s_style_btn_primary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_primary, 6);

    /* Cancel is a calm, deliberate "abandon and restart" action - neutral
     * grey, distinct from the primary orange (Start/Cooling) and the
     * danger-red Emergency Stop pinned in the sidebar. */
    lv_style_init(&s_style_btn_secondary);
    lv_style_set_bg_color(&s_style_btn_secondary, lv_color_hex(0x616161));
    lv_style_set_bg_opa(&s_style_btn_secondary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_secondary, 6);

    lv_style_init(&s_style_btn_label);
    lv_style_set_text_color(&s_style_btn_label, lv_color_hex(0xFFFFFF));

    /* Small numeric labels marking each segment's own target temp/fan
     * value on the dashed overlay curve (rebuild_target_lines()) - the
     * chart otherwise has no numeric markings at all. Colors match their
     * respective dashed line. */
    lv_style_init(&s_style_seg_temp_label);
    lv_style_set_text_color(&s_style_seg_temp_label, lv_color_hex(0xFF9746));
    lv_style_set_text_font(&s_style_seg_temp_label, &lv_font_montserrat_12);

    lv_style_init(&s_style_seg_fan_label);
    lv_style_set_text_color(&s_style_seg_fan_label, lv_color_hex(0x66BB6A));
    lv_style_set_text_font(&s_style_seg_fan_label, &lv_font_montserrat_12);

    s_styles_ready = true;
}

static void reset_chart(void)
{
    if (s_chart == NULL) {
        return;
    }
    lv_chart_set_all_value(s_chart, s_chart_bt_series, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s_chart, s_chart_fan_series, LV_CHART_POINT_NONE);
}

/* roast_dashboard_show_in() tears down and recreates this whole screen
 * (including a brand-new, blank lv_chart_t) every time the operator
 * switches back to the Roast tab - if a roast is already in progress (e.g.
 * they checked History/Presets/Config mid-roast), the fresh chart would
 * otherwise stay empty except for one new point at the current elapsed
 * time, even though telemetry recording never stopped in the background
 * (roast_telemetry_service.c). Re-fill it from the session's own JSONL
 * history file (same format/parsing as session_review.c's replay) so
 * returning to this tab always shows the full curve so far. */
static void replay_current_session_into_chart(void)
{
    if (s_chart == NULL || s_chart_bt_series == NULL || s_chart_fan_series == NULL) {
        return;
    }
    const roast_session_t *session = session_sm_get_state();
    if (session->session_id[0] == '\0') {
        return;
    }
    FILE *f = session_store_open_session(session->session_id);
    if (f == NULL) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            int idx = (int)((t / 1000) * (MAX_CHART_POINTS - 1) / (s_chart_duration_s > 0 ? s_chart_duration_s : 1));
            if (idx < 0) idx = 0;
            if (idx >= MAX_CHART_POINTS) idx = MAX_CHART_POINTS - 1;
            lv_chart_set_value_by_id(s_chart, s_chart_bt_series, (uint16_t)idx, (lv_coord_t)bt);
            lv_chart_set_value_by_id(s_chart, s_chart_fan_series, (uint16_t)idx, (lv_coord_t)fan);
        }
    }
    fclose(f);
    lv_chart_refresh(s_chart);
}

/* Recomputes the dashed target-curve overlays from the currently loaded
 * preset. Must run AFTER the chart's real pixel size is known (i.e. after
 * an lv_obj_update_layout() pass), since the overlay points are mapped
 * directly to the chart's own pixel bounding box. */
static void rebuild_target_lines(void)
{
    if (s_chart == NULL || s_target_temp_line == NULL || s_target_fan_line == NULL) {
        return;
    }

    /* Clear any segment labels from a previous call - see the comment on
     * s_segment_labels above for why this can no longer assume "only ever
     * called once". */
    for (int i = 0; i < s_segment_label_count; i++) {
        if (s_segment_labels[i] != NULL) {
            lv_obj_del(s_segment_labels[i]);
            s_segment_labels[i] = NULL;
        }
    }
    s_segment_label_count = 0;

    if (!s_has_profile) {
        lv_obj_add_flag(s_target_temp_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_target_fan_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_coord_t chart_w = lv_obj_get_width(s_chart);
    lv_coord_t chart_h = lv_obj_get_height(s_chart);
    if (chart_w <= 0 || chart_h <= 0) {
        return;
    }

    /* lv_chart's OWN series points are inset from the widget's box by its
     * padding (see lv_chart_get_point_pos_by_id() in lv_chart.c: point 0 is
     * at x=pad_left, the last point is at x=pad_left+content_width, not at
     * the raw 0/chart_w edges). Our overlay lines are plain lv_line
     * children of the chart, and lv_obj_move_to() ALREADY offsets a child's
     * local (0,0) by the parent's pad_left/pad_top (see lv_obj_pos.c) since
     * we position them at lv_obj_set_pos(line, 0, 0) - so adding pad_left/
     * pad_top again here double-counts the inset (this was tried and caused
     * a large gap on the left plus overflow on the right). The points below
     * must therefore be in the chart's CONTENT-box-local space (0..content_w,
     * 0..content_h), matching the same box the real series is drawn into,
     * without re-adding the padding a second time. */
    lv_coord_t content_w = lv_obj_get_content_width(s_chart);
    lv_coord_t content_h = lv_obj_get_content_height(s_chart);

    for (int i = 0; i < MAX_CHART_POINTS; i++) {
        uint32_t t = (uint32_t)((uint64_t)i * s_chart_duration_s / (MAX_CHART_POINTS - 1));
        float target_temp = roast_profile_get_target_temp_c(&s_active_profile, t);
        uint8_t target_fan = roast_profile_get_target_fan_pct(&s_active_profile, t);

        lv_coord_t x = (lv_coord_t)((uint64_t)i * content_w / (MAX_CHART_POINTS - 1));

        float temp_frac = target_temp / 260.0f; /* Matches the chart's primary Y range (0-260C, FR-026). */
        if (temp_frac < 0.0f) temp_frac = 0.0f;
        if (temp_frac > 1.0f) temp_frac = 1.0f;
        s_target_temp_points[i].x = x;
        s_target_temp_points[i].y = content_h - (lv_coord_t)(temp_frac * content_h);

        float fan_frac = (float)target_fan / 100.0f; /* Matches the chart's secondary Y range (0-100%). */
        if (fan_frac < 0.0f) fan_frac = 0.0f;
        if (fan_frac > 1.0f) fan_frac = 1.0f;
        s_target_fan_points[i].x = x;
        s_target_fan_points[i].y = content_h - (lv_coord_t)(fan_frac * content_h);
    }

    lv_obj_set_size(s_target_temp_line, content_w, content_h);
    lv_obj_set_size(s_target_fan_line, content_w, content_h);
    lv_line_set_points(s_target_temp_line, s_target_temp_points, MAX_CHART_POINTS);
    lv_line_set_points(s_target_fan_line, s_target_fan_points, MAX_CHART_POINTS);
    lv_obj_clear_flag(s_target_temp_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_target_fan_line, LV_OBJ_FLAG_HIDDEN);

    /* Numeric value labels at each segment's own setpoint (operator
     * request: the chart had no numeric markings at all) - one small label
     * per segment for temp (orange) and fan (green), centered over that
     * segment's flat step. Tracked in s_segment_labels[] and cleared at the
     * top of this function on every call (see above). */
    uint32_t seg_cursor_s = 0;
    for (uint8_t i = 0; i < s_active_profile.point_count; i++) {
        const roast_profile_point_t *pt = &s_active_profile.points[i];
        uint32_t seg_mid_s = seg_cursor_s + pt->duration_s / 2;
        seg_cursor_s += pt->duration_s;

        lv_coord_t x = (lv_coord_t)((uint64_t)seg_mid_s * content_w / (s_chart_duration_s > 0 ? s_chart_duration_s : 1));
        if (x < 0) x = 0;
        if (x > content_w) x = content_w;

        float temp_frac = pt->target_temp_c / 260.0f;
        if (temp_frac < 0.0f) temp_frac = 0.0f;
        if (temp_frac > 1.0f) temp_frac = 1.0f;
        lv_coord_t temp_y = content_h - (lv_coord_t)(temp_frac * content_h);

        char seg_buf[8];
        if (s_segment_label_count < MAX_SEGMENT_LABELS) {
            lv_obj_t *temp_lbl = lv_label_create(s_chart);
            lv_obj_add_style(temp_lbl, &s_style_seg_temp_label, LV_PART_MAIN);
            snprintf(seg_buf, sizeof(seg_buf), "%.0f", (double)pt->target_temp_c);
            lv_label_set_text(temp_lbl, seg_buf);
            lv_obj_set_pos(temp_lbl, x - 8, temp_y - 16);
            s_segment_labels[s_segment_label_count++] = temp_lbl;
        }

        float fan_frac = (float)pt->target_fan_pct / 100.0f;
        if (fan_frac < 0.0f) fan_frac = 0.0f;
        if (fan_frac > 1.0f) fan_frac = 1.0f;
        lv_coord_t fan_y = content_h - (lv_coord_t)(fan_frac * content_h);

        if (s_segment_label_count < MAX_SEGMENT_LABELS) {
            lv_obj_t *fan_lbl = lv_label_create(s_chart);
            lv_obj_add_style(fan_lbl, &s_style_seg_fan_label, LV_PART_MAIN);
            snprintf(seg_buf, sizeof(seg_buf), "%u%%", (unsigned)pt->target_fan_pct);
            lv_label_set_text(fan_lbl, seg_buf);
            lv_obj_set_pos(fan_lbl, x - 8, fan_y + 4);
            s_segment_labels[s_segment_label_count++] = fan_lbl;
        }
    }
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

static bool phase_is_idle_like(roast_phase_t phase)
{
    return phase == ROAST_PHASE_IDLE || phase == ROAST_PHASE_COMPLETED || phase == ROAST_PHASE_ABORTED;
}

/* Reloads the active profile/duration/preset-name label and rebuilds the
 * target-curve overlay whenever the OPERATOR selects a different preset
 * elsewhere (Presets tab, or the web /presets page) while this screen is
 * already showing - previously the dashboard only ever loaded the
 * selected profile once, at show-time, so a web-side preset change never
 * reflected here until the tab was left and re-entered. Only called while
 * idle (see refresh_timer_cb) - profile_store now refuses a preset switch
 * during an active roast (profile_store_set_selected()), so this can never
 * fire mid-roast and disrupt an in-progress curve. */
static void sync_selected_profile_if_changed(void)
{
    int selected_id = -1;
    profile_store_get_selected_id(&selected_id);
    if (selected_id == s_last_known_selected_id) {
        return;
    }
    s_last_known_selected_id = selected_id;

    s_has_profile = (profile_store_get_selected(&s_active_profile) == ESP_OK);
    s_chart_duration_s = s_has_profile ? roast_profile_total_duration_s(&s_active_profile) : 0;
    if (s_chart_duration_s == 0) {
        s_chart_duration_s = DEFAULT_CHART_DURATION_S;
    }
    if (s_preset_label != NULL) {
        lv_label_set_text(s_preset_label, s_has_profile ? s_active_profile.name : "No preset selected");
    }
    rebuild_target_lines();
}

static void start_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const roast_session_t *session = session_sm_get_state();
    if (phase_is_idle_like(session->phase)) {
        /* T034: a selected preset always runs as a Profile-mode session (its
         * target fan/heater curve is auto-driven by profile_curve_follower.c);
         * with no preset selected, fall back to plain Manual/Artisan control. */
        roast_control_mode_t mode = s_has_profile ? ROAST_MODE_PROFILE : ROAST_MODE_MANUAL_ARTISAN;
        esp_err_t err = session_sm_start(mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "session_sm_start failed: %s", esp_err_to_name(err));
        } else {
            reset_chart();
            roast_telemetry_service_on_roast_started();
        }
    }
}

static void charge_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    command_dispatcher_confirm_charge(SAFETY_CMD_SOURCE_DISPLAY);
}

static void pause_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const roast_session_t *session = session_sm_get_state();
    if (session->paused) {
        command_dispatcher_resume_session(SAFETY_CMD_SOURCE_DISPLAY);
    } else {
        command_dispatcher_pause_session(SAFETY_CMD_SOURCE_DISPLAY);
    }
}

static void cancel_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    command_dispatcher_cancel_session(SAFETY_CMD_SOURCE_DISPLAY);
}

/* T029: direct fan/heater sliders live in the dedicated Manual tab
 * (screens/manual_control.c) instead of here. */

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    roast_telemetry_snapshot_t snap;
    roast_telemetry_service_get_snapshot(&snap);
    bool active = !phase_is_idle_like(snap.phase);
    if (!active) {
        sync_selected_profile_if_changed();
    }
    /* Chart/curve timeline only makes sense once CHARGE has been confirmed
     * (ROASTING/DEVELOPMENT/COOLING) - PREHEAT elapsed time is a different,
     * unrelated clock (time since Start Roast was tapped, not since
     * CHARGE), so it's deliberately excluded from the plotted curve. */
    bool curve_active = (snap.phase == ROAST_PHASE_ROASTING || snap.phase == ROAST_PHASE_DEVELOPMENT ||
                          snap.phase == ROAST_PHASE_COOLING);
    /* TBT should show during PREHEAT too (operator request: preheat now
     * actually heats toward the first setpoint's target - see
     * profile_curve_follower.c) - held flat at segment 0's target since
     * PREHEAT has no CHARGE-relative elapsed-time reference yet. */
    bool tbt_visible = (curve_active || snap.phase == ROAST_PHASE_PREHEAT) && s_has_profile;

    char buf[48];

    if (snap.sensor_valid) {
        if (tbt_visible) {
            /* Target Bean Temperature (TBT): whichever segment the profile
             * curve is currently in (step transition, no ramp - see
             * roast_profile.c) - shown alongside the real measured BT so
             * the operator can see both at a glance. */
            int64_t tbt_elapsed_s = curve_active ? snap.elapsed_ms / 1000 : 0;
            if (tbt_elapsed_s < 0) {
                tbt_elapsed_s = 0;
            }
            float target_temp = roast_profile_get_target_temp_c(&s_active_profile, (uint32_t)tbt_elapsed_s);
            snprintf(buf, sizeof(buf), "BT: %.1f C / TBT %.1f C", snap.bean_temp_c, target_temp);
        } else {
            snprintf(buf, sizeof(buf), "BT: %.1f C", snap.bean_temp_c);
        }
    } else {
        snprintf(buf, sizeof(buf), "BT: --");
    }
    lv_label_set_text(s_bt_label, buf);

    if (active && snap.ror_c_per_min != 0.0f) {
        snprintf(buf, sizeof(buf), "RoR: %.1f C/min", snap.ror_c_per_min);
    } else {
        snprintf(buf, sizeof(buf), "RoR: --");
    }
    lv_label_set_text(s_ror_label, buf);

    if (snap.dtr_pct >= 0.0f) {
        snprintf(buf, sizeof(buf), "DTR: %.0f%%", snap.dtr_pct);
    } else {
        snprintf(buf, sizeof(buf), "DTR: --");
    }
    lv_label_set_text(s_dtr_label, buf);

    snprintf(buf, sizeof(buf), "Fan: %d%%", snap.fan_pct);
    lv_label_set_text(s_fan_label, buf);

    snprintf(buf, sizeof(buf), "Heater: %d%%", snap.heater_pct);
    lv_label_set_text(s_heater_label, buf);

    const char *phase_str = phase_text(snap.phase);
    if (snap.paused) {
        snprintf(buf, sizeof(buf), "%s (PAUSED)", phase_str);
    } else {
        snprintf(buf, sizeof(buf), "%s", phase_str);
    }
    lv_label_set_text(s_phase_label, buf);

    int64_t elapsed_s = snap.elapsed_ms / 1000;
    if (elapsed_s < 0) {
        elapsed_s = 0;
    }
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(elapsed_s / 60), (int)(elapsed_s % 60));
    lv_label_set_text(s_timer_label, buf);

    /* Wi-Fi IP, top-right of the screen - refreshed every tick since the
     * connection can come up (or drop) after this screen was first shown. */
    char ip_buf[24];
    if (wifi_provisioning_get_ip_str(ip_buf, sizeof(ip_buf)) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Wi-Fi: %s", ip_buf);
    } else {
        snprintf(buf, sizeof(buf), "Wi-Fi: not connected");
    }
    lv_label_set_text(s_wifi_ip_label, buf);

    /* Draw the live BT/Fan curve (T063) against the preset's target curve
     * (rebuild_target_lines(), drawn once at show-time). Fixed-timeline
     * chart spanning the preset's exact total duration: index maps to
     * elapsed time, so the curve fills in left-to-right as the roast
     * progresses instead of scrolling. */
    if (curve_active && s_chart != NULL) {
        int idx = (int)((int64_t)elapsed_s * (MAX_CHART_POINTS - 1) / (s_chart_duration_s > 0 ? s_chart_duration_s : 1));
        if (idx >= MAX_CHART_POINTS) {
            idx = MAX_CHART_POINTS - 1;
        }
        if (idx >= 0) {
            if (snap.sensor_valid) {
                lv_chart_set_value_by_id(s_chart, s_chart_bt_series, (uint16_t)idx, (lv_coord_t)snap.bean_temp_c);
            }
            lv_chart_set_value_by_id(s_chart, s_chart_fan_series, (uint16_t)idx, (lv_coord_t)snap.fan_pct);
        }
    }

    /* Toggle button visibility/labels based on current phase: idle shows
     * Start Roast; PREHEAT shows Charge+Cancel (T036 - operator confirms
     * beans loaded, which is what actually starts the roast clock/curve);
     * anything past that (ROASTING/DEVELOPMENT/COOLING) shows Pause/Resume
     * + Cancel, same as before. */
    if (phase_is_idle_like(snap.phase)) {
        lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_charge_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_active_group, LV_OBJ_FLAG_HIDDEN);
    } else if (snap.phase == ROAST_PHASE_PREHEAT) {
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_charge_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_active_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* ROASTING/DEVELOPMENT/COOLING - all handled automatically now
         * (profile curve / Cancel-Stop safety cooling), so just Pause/Resume
         * + Cancel remain available; Cancel while already COOLING simply
         * marks the eventual outcome as ABORTED instead of COMPLETED
         * (session_sm_cancel() is idempotent). */
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_charge_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_active_group, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_pause_btn_label, snap.paused ? LV_SYMBOL_PLAY " Resume" : LV_SYMBOL_PAUSE " Pause");
    }
}

/* Creates a transparent full-width flex row (space-between, vertically
 * centered) inside `col_parent`. Used for the info panel below the chart so
 * rows are stacked automatically by LVGL's layout engine instead of manual
 * y-coordinates - immune to font-metric-driven overlap bugs. */
static lv_obj_t *make_flex_row(lv_obj_t *col_parent, lv_coord_t width)
{
    lv_obj_t *row = lv_obj_create(col_parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

void roast_dashboard_show_in(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);
    const lv_coord_t content_h = lv_obj_get_height(parent);

    /* Load whichever preset is currently selected (Presets tab) - the Roast
     * screen always runs the selected preset: its target BT/Fan curves are
     * what get plotted (dashed) against the live measured values, and the
     * chart's timeline spans exactly its total duration. Falls back to a
     * generic 20-minute window with no target overlay if none is selected. */
    s_has_profile = (profile_store_get_selected(&s_active_profile) == ESP_OK);
    s_chart_duration_s = s_has_profile ? roast_profile_total_duration_s(&s_active_profile) : 0;
    if (s_chart_duration_s == 0) {
        s_chart_duration_s = DEFAULT_CHART_DURATION_S;
    }
    profile_store_get_selected_id(&s_last_known_selected_id); /* Keeps sync_selected_profile_if_changed() from redundantly reloading on the very next idle tick. */

    /* Root column: preset name + chart (grows to fill whatever space isn't
     * used by the info rows / bottom button row below it) + info panel +
     * bottom button row, all stacked by the flex layout. This guarantees
     * the chart automatically grows to fill the gap left when the bottom
     * row is hidden (IDLE state) instead of leaving blank space, and never
     * overlaps it when it's shown (active state) - no manual pixel math.
     * Created as the sole child of `parent` (the shared nav_shell content
     * pane) so nothing about this layout leaks into other tabs once
     * nav_shell cleans this container away on tab switch. */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, content_w, content_h);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 6, LV_PART_MAIN);

    /* Header row: selected preset name (left) + Wi-Fi IP (right), per the
     * operator's request. */
    lv_obj_t *header_row = make_flex_row(root, content_w - 16);
    s_preset_label = lv_label_create(header_row);
    lv_obj_add_style(s_preset_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_preset_label, s_has_profile ? s_active_profile.name : "No preset selected");

    s_wifi_ip_label = lv_label_create(header_row);
    lv_obj_add_style(s_wifi_ip_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_wifi_ip_label, "Wi-Fi: --");

    /* Live BT+Fan timeline chart (T063), on top per the approved sketch.
     * flex_grow=1 makes it consume all leftover vertical space. Solid lines
     * are the actual measured values; the dashed target-curve overlays
     * (rebuild_target_lines(), below) are plain lv_line children of this
     * chart since lv_chart's own line drawing ignores dash style props. */
    s_chart = lv_chart_create(root);
    lv_obj_set_width(s_chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_chart, 1);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    /* The default theme's panel padding (16-24px per side, DPI-dependent)
     * is much larger than this small 480x272 display needs and is what
     * made the plotted curves look like they had a huge margin - shrink it
     * to a small fixed inset instead. */
    lv_obj_set_style_pad_all(s_chart, 4, LV_PART_MAIN);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 3, 4);
    lv_chart_set_point_count(s_chart, MAX_CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 260);    /* BT: 0-260C (FR-026 absolute cutoff). */
    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);  /* Fan: 0-100%. */
    s_chart_bt_series = lv_chart_add_series(s_chart, lv_color_hex(0xFF9746), LV_CHART_AXIS_PRIMARY_Y);
    s_chart_fan_series = lv_chart_add_series(s_chart, lv_color_hex(0x66BB6A), LV_CHART_AXIS_SECONDARY_Y);
    reset_chart();
    /* This screen (and its chart) gets torn down/rebuilt every time the
     * operator switches tabs - if a roast is already running, re-fill the
     * fresh chart from what's already been recorded so far instead of
     * leaving it blank (see replay_current_session_into_chart()). */
    {
        const roast_session_t *session = session_sm_get_state();
        if (session->phase == ROAST_PHASE_ROASTING || session->phase == ROAST_PHASE_DEVELOPMENT ||
            session->phase == ROAST_PHASE_COOLING) {
            replay_current_session_into_chart();
        }
    }

    s_target_temp_line = lv_line_create(s_chart);
    lv_obj_set_style_line_color(s_target_temp_line, lv_color_hex(0xFF9746), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_target_temp_line, 2, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(s_target_temp_line, 5, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(s_target_temp_line, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_target_temp_line, false, LV_PART_MAIN);
    lv_obj_clear_flag(s_target_temp_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_target_temp_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_target_temp_line, 0, 0);

    s_target_fan_line = lv_line_create(s_chart);
    lv_obj_set_style_line_color(s_target_fan_line, lv_color_hex(0x66BB6A), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_target_fan_line, 2, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(s_target_fan_line, 5, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(s_target_fan_line, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_target_fan_line, false, LV_PART_MAIN);
    lv_obj_clear_flag(s_target_fan_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_target_fan_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_target_fan_line, 0, 0);

    /* Info panel: a flex column of rows (phase/timer, BT/Fan, RoR/Heater,
     * DTR/Start-Pause) stacked automatically below the chart. Height is
     * LV_SIZE_CONTENT (hugs its rows exactly). */
    lv_obj_t *info_panel = lv_obj_create(root);
    lv_obj_remove_style_all(info_panel);
    lv_obj_set_size(info_panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info_panel, 3, LV_PART_MAIN);

    lv_obj_t *row_phase = make_flex_row(info_panel, content_w - 16);
    s_phase_label = lv_label_create(row_phase);
    lv_obj_add_style(s_phase_label, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(s_phase_label, "IDLE");
    s_timer_label = lv_label_create(row_phase);
    lv_obj_add_style(s_timer_label, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(s_timer_label, "00:00");

    lv_obj_t *row_bt = make_flex_row(info_panel, content_w - 16);
    s_bt_label = lv_label_create(row_bt);
    lv_obj_add_style(s_bt_label, &s_style_value, LV_PART_MAIN);
    lv_label_set_text(s_bt_label, "BT: --");
    s_fan_label = lv_label_create(row_bt);
    lv_obj_add_style(s_fan_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_fan_label, "Fan: 0%");

    lv_obj_t *row_ror = make_flex_row(info_panel, content_w - 16);
    s_ror_label = lv_label_create(row_ror);
    lv_obj_add_style(s_ror_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_ror_label, "RoR: --");
    s_heater_label = lv_label_create(row_ror);
    lv_obj_add_style(s_heater_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_heater_label, "Heater: 0%");

    /* DTR + Start Roast / Pause-Resume share a row: whichever button applies
     * to the current phase sits in the free space to the right of DTR. */
    lv_obj_t *row_dtr = make_flex_row(info_panel, content_w - 16);
    s_dtr_label = lv_label_create(row_dtr);
    lv_obj_add_style(s_dtr_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_dtr_label, "DTR: --");

    /* Start Roast button (visible when IDLE/COMPLETED/ABORTED). */
    s_start_btn = lv_btn_create(row_dtr);
    lv_obj_add_style(s_start_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(s_start_btn, 130, 24);
    lv_obj_add_event_cb(s_start_btn, start_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_start_btn_label = lv_label_create(s_start_btn);
    lv_obj_add_style(s_start_btn_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(s_start_btn_label, LV_SYMBOL_PLAY " Start Roast");
    lv_obj_center(s_start_btn_label);

    /* Charge group (T036 minimal): shown only during PREHEAT. Tapping
     * Charge confirms beans are loaded and transitions PREHEAT->ROASTING,
     * which is also when the roast clock/curve timeline actually starts
     * (see session_sm_confirm_charge()). Grouped with its own small Cancel
     * button so the operator can still back out during preheat, same
     * fixed-size-container reasoning as `active_group` above. */
    lv_obj_t *charge_group = lv_obj_create(row_dtr);
    lv_obj_remove_style_all(charge_group);
    lv_obj_set_size(charge_group, 164, 24);
    lv_obj_clear_flag(charge_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(charge_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(charge_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(charge_group, 6, LV_PART_MAIN);
    lv_obj_add_flag(charge_group, LV_OBJ_FLAG_HIDDEN);
    s_charge_group = charge_group;

    s_charge_cancel_btn = lv_btn_create(charge_group);
    lv_obj_add_style(s_charge_cancel_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(s_charge_cancel_btn, 28, 24);
    lv_obj_add_event_cb(s_charge_cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *charge_cancel_label = lv_label_create(s_charge_cancel_btn);
    lv_obj_add_style(charge_cancel_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(charge_cancel_label, LV_SYMBOL_CLOSE);
    lv_obj_center(charge_cancel_label);

    s_charge_btn = lv_btn_create(charge_group);
    lv_obj_add_style(s_charge_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(s_charge_btn, 130, 24);
    lv_obj_add_event_cb(s_charge_btn, charge_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_charge_btn_label = lv_label_create(s_charge_btn);
    lv_obj_add_style(s_charge_btn_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(s_charge_btn_label, LV_SYMBOL_OK " Charge");
    lv_obj_center(s_charge_btn_label);

    /* Pause/Resume button (visible during an active session), grouped
     * tightly with a small, discreet Cancel button to its left - both live
     * in `active_group` so they're toggled together as a unit against
     * `start_btn` (idle) in the same row_dtr slot. */
    /* NOTE: fixed pixel size (28 + 6 pad + 130 = 164) instead of
     * LV_SIZE_CONTENT - a content-sized flex container whose children are
     * added while it's still hidden (its initial state here) isn't
     * reliably re-measured by LVGL8's flex engine once later unhidden from
     * refresh_timer_cb, which was making the Cancel button vanish
     * entirely. An explicit fixed size sidesteps that timing issue. */
    lv_obj_t *active_group = lv_obj_create(row_dtr);
    lv_obj_remove_style_all(active_group);
    lv_obj_set_size(active_group, 164, 24);
    lv_obj_clear_flag(active_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(active_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(active_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(active_group, 6, LV_PART_MAIN);
    lv_obj_add_flag(active_group, LV_OBJ_FLAG_HIDDEN);
    s_active_group = active_group;

    /* Cancel: small, icon-only, neutral grey - discreet on purpose, distinct
     * from the primary Pause/Resume and the danger-red sidebar E-Stop. */
    s_cancel_btn = lv_btn_create(active_group);
    lv_obj_add_style(s_cancel_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(s_cancel_btn, 28, 24);
    lv_obj_add_event_cb(s_cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(s_cancel_btn);
    lv_obj_add_style(cancel_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE);
    lv_obj_center(cancel_label);

    s_pause_btn = lv_btn_create(active_group);
    lv_obj_add_style(s_pause_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(s_pause_btn, 130, 24);
    lv_obj_add_event_cb(s_pause_btn, pause_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_pause_btn_label = lv_label_create(s_pause_btn);
    lv_obj_add_style(s_pause_btn_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(s_pause_btn_label, LV_SYMBOL_PAUSE " Pause");
    lv_obj_center(s_pause_btn_label);

    /* NOTE: no manual Start Cooling/Complete button anymore - cooling now
     * starts automatically (either the profile's own trailing "cooling"
     * segment(s) via profile_curve_follower.c, or immediately when the
     * operator cancels/emergency-stops - see session_sm_cancel()) and ends
     * automatically too (profile timeline elapsed, BT drops below the safe
     * threshold, or a failsafe max duration - all in profile_curve_follower.c). */

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);

    /* Force layout resolution NOW so the chart's real pixel size is known
     * before mapping the target-curve overlay points to it (lesson learned
     * from the earlier "blank on first boot" bug: flex_grow sizes aren't
     * resolved until a layout pass runs). */
    lv_obj_update_layout(root);
    rebuild_target_lines();

    /* Paint immediately instead of waiting for the first 500ms tick. */
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "Roast dashboard shown (preset=%s, duration=%lus)",
             s_has_profile ? s_active_profile.name : "none", (unsigned long)s_chart_duration_s);
}

void roast_dashboard_hide(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    s_chart = NULL;
    s_chart_bt_series = NULL;
    s_chart_fan_series = NULL;
    s_target_temp_line = NULL;
    s_target_fan_line = NULL;
    /* The segment labels (children of s_chart) are already destroyed by
     * nav_shell's lv_obj_clean(s_content) right after this hide_fn runs -
     * just forget our now-dangling pointers instead of calling lv_obj_del()
     * on them again next time rebuild_target_lines() runs for a new chart. */
    s_segment_label_count = 0;
}

/**
 * @file session_review.c
 * @brief Roast history/review screen implementation (see header).
 *
 * Reads back the JSON-lines telemetry format written by
 * roast_core/roast_telemetry_service.c during an active roast:
 *   {"t":<elapsed_ms>,"bt":<C>,"ror":<C/min>,"fan":<pct>,"heater":<pct>,"phase":<int>}
 * plus the per-session metadata (storage/session_store.h session_meta_t:
 * sequential roast number + a full snapshot of whichever preset was
 * selected when the roast started, FR-034), used to reconstruct the same
 * target-vs-actual chart style as the live Roast dashboard.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "roast_core/roast_profile.h"
#include "storage/session_store.h"
#include "ui_display/screens/session_review.h"

static const char *TAG = "session_review";

#define MAX_SESSIONS_LISTED 30
#define MAX_CHART_POINTS 100
#define FALLBACK_DURATION_S 60

static lv_obj_t *s_content_parent;
static lv_obj_t *s_delete_all_btn;
static lv_obj_t *s_delete_all_lbl;
static bool s_delete_all_armed = false;
static lv_timer_t *s_disarm_timer;

static lv_style_t s_style_label;
static lv_style_t s_style_stat_label;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_danger;
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
    lv_style_set_text_color(&s_style_label, lv_color_hex(0xe0e0e0));

    lv_style_init(&s_style_stat_label);
    lv_style_set_text_color(&s_style_stat_label, lv_color_hex(0x9e9e9e));

    lv_style_init(&s_style_btn);
    lv_style_set_bg_color(&s_style_btn, lv_color_hex(0xFF9746));
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn, 6);

    lv_style_init(&s_style_btn_danger);
    lv_style_set_bg_color(&s_style_btn_danger, lv_color_hex(0xB3261E));
    lv_style_set_bg_opa(&s_style_btn_danger, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_danger, 6);

    lv_style_init(&s_style_btn_label);
    lv_style_set_text_color(&s_style_btn_label, lv_color_hex(0xFFFFFF));

    /* Small numeric labels marking each segment's own target temp/fan value
     * on the dashed overlay curve - matches roast_dashboard.c's live chart. */
    lv_style_init(&s_style_seg_temp_label);
    lv_style_set_text_color(&s_style_seg_temp_label, lv_color_hex(0xFF9746));
    lv_style_set_text_font(&s_style_seg_temp_label, &lv_font_montserrat_12);

    lv_style_init(&s_style_seg_fan_label);
    lv_style_set_text_color(&s_style_seg_fan_label, lv_color_hex(0x66BB6A));
    lv_style_set_text_font(&s_style_seg_fan_label, &lv_font_montserrat_12);

    s_styles_ready = true;
}

static void show_list(lv_obj_t *parent);

/* Same flex-row helper pattern as roast_dashboard.c: a transparent,
 * non-scrollable, full-width row with space-between main-axis alignment, so
 * rows stack automatically via the parent's flex layout instead of manual
 * pixel y-coordinates (which was the root cause of the messy/overlapping
 * detail-screen layout and the stray graph scrollbar - manually-aligned
 * content easily overflows the 272px display height with no warning). */
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

static void back_to_list_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_clean(s_content_parent);
    show_list(s_content_parent);
}

/* Builds a display name for a session id: "Roast #<N> - <Preset>" when
 * metadata exists (every session recorded from now on), falling back to the
 * raw session id for older sessions recorded before this feature existed. */
static void format_session_display_name(const char *session_id, char *out, size_t out_len)
{
    session_meta_t meta;
    if (session_store_load_meta(session_id, &meta) == ESP_OK) {
        if (meta.has_profile) {
            snprintf(out, out_len, "Roast #%lu - %s", (unsigned long)meta.roast_number, meta.profile.name);
        } else {
            snprintf(out, out_len, "Roast #%lu", (unsigned long)meta.roast_number);
        }
    } else {
        strncpy(out, session_id, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static void show_session_detail(lv_obj_t *parent, const char *session_id)
{
    lv_obj_clean(parent);
    ensure_styles();

    char display_name[80];
    format_session_display_name(session_id, display_name, sizeof(display_name));

    /* Flex-column root filling the whole content pane, non-scrollable -
     * every element below is stacked/sized by the layout engine instead of
     * manual lv_obj_align() pixel offsets, so nothing can silently overflow
     * the 272px display height (the cause of the earlier messy layout +
     * stray scrollbar). */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 4, LV_PART_MAIN);

    const lv_coord_t content_w = lv_obj_get_width(parent) - 16;

    /* Header row: title (ellipsized if long) + Back button, side by side. */
    lv_obj_t *header_row = make_flex_row(root, content_w);
    lv_obj_t *title = lv_label_create(header_row);
    lv_obj_add_style(title, &s_style_label, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, display_name);

    lv_obj_t *back_btn = lv_btn_create(header_row);
    lv_obj_add_style(back_btn, &s_style_btn, LV_PART_MAIN);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_add_event_cb(back_btn, back_to_list_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_obj_add_style(back_label, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    session_meta_t meta;
    bool has_meta = (session_store_load_meta(session_id, &meta) == ESP_OK);
    bool has_profile = has_meta && meta.has_profile;

    FILE *f = session_store_open_session(session_id);
    if (f == NULL) {
        lv_obj_t *err_label = lv_label_create(root);
        lv_obj_add_style(err_label, &s_style_label, LV_PART_MAIN);
        lv_label_set_text(err_label, "Could not open session file");
        return;
    }

    /* First pass: overall stats (total time, peaks) needed both for the
     * chart's timeline span (when no preset snapshot exists to define
     * it) and for the stats panel below the chart. */
    char line[160];
    int64_t max_t_ms = 0;
    float max_bt = 0.0f, max_ror = 0.0f;
    long heater_sum = 0, fan_sum = 0;
    size_t sample_count = 0;
    bool any_sample = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            if (t > max_t_ms) {
                max_t_ms = t;
            }
            if (bt > max_bt) {
                max_bt = bt;
            }
            if (ror > max_ror) {
                max_ror = ror;
            }
            heater_sum += heater;
            fan_sum += fan;
            sample_count++;
            any_sample = true;
        }
    }

    if (!any_sample) {
        fclose(f);
        lv_obj_t *empty_label = lv_label_create(root);
        lv_obj_add_style(empty_label, &s_style_label, LV_PART_MAIN);
        lv_label_set_text(empty_label, "No telemetry recorded for this session");
        return;
    }

    uint32_t duration_s = has_profile ? roast_profile_total_duration_s(&meta.profile) : 0;
    if (duration_s == 0) {
        duration_s = (uint32_t)(max_t_ms / 1000);
    }
    if (duration_s == 0) {
        duration_s = FALLBACK_DURATION_S;
    }

    /* Chart grows to fill whatever vertical space is left over after the
     * header/legend/stats rows below it - same technique as the live
     * dashboard, avoids any manual height math (and the overflow that
     * caused it). */
    lv_obj_t *chart = lv_chart_create(root);
    lv_obj_set_width(chart, content_w);
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    /* Shrink the theme's oversized default panel padding - see the matching
     * comment in roast_dashboard.c's chart setup. */
    lv_obj_set_style_pad_all(chart, 4, LV_PART_MAIN);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 3, 4);
    lv_chart_set_point_count(chart, MAX_CHART_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 260);   /* BT: 0-260C (FR-026 absolute cutoff). */
    lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100); /* Fan: 0-100%. */
    lv_chart_series_t *bt_series = lv_chart_add_series(chart, lv_color_hex(0xFF9746), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *fan_series = lv_chart_add_series(chart, lv_color_hex(0x66BB6A), LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_value(chart, bt_series, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart, fan_series, LV_CHART_POINT_NONE);

    /* Second pass: plot the actual measured curve, indexed by each
     * line's own recorded elapsed time (not line position) so it
     * lines up exactly with the target overlay below. */
    rewind(f);
    while (fgets(line, sizeof(line), f) != NULL) {
        long long t = 0;
        float bt = 0.0f, ror = 0.0f;
        int fan = 0, heater = 0, phase = 0;
        if (sscanf(line, "{\"t\":%lld,\"bt\":%f,\"ror\":%f,\"fan\":%d,\"heater\":%d,\"phase\":%d}",
                   &t, &bt, &ror, &fan, &heater, &phase) >= 3) {
            int idx = (int)((t / 1000) * (MAX_CHART_POINTS - 1) / duration_s);
            if (idx < 0) idx = 0;
            if (idx >= MAX_CHART_POINTS) idx = MAX_CHART_POINTS - 1;
            lv_chart_set_value_by_id(chart, bt_series, (uint16_t)idx, (lv_coord_t)bt);
            lv_chart_set_value_by_id(chart, fan_series, (uint16_t)idx, (lv_coord_t)fan);
        }
    }
    fclose(f);
    lv_chart_refresh(chart);

    /* Force layout resolution NOW so the chart's real pixel size (needed to
     * map the target-curve overlay points) is known before rebuilding the
     * dashed lines below - same lesson as the live dashboard's boot-blank
     * bug: flex_grow sizes aren't resolved until a layout pass runs. */
    lv_obj_update_layout(root);
    lv_coord_t chart_w = lv_obj_get_width(chart);
    lv_coord_t chart_h = lv_obj_get_height(chart);

    /* Target curve overlay (dashed), same convention as the live
     * dashboard: plain lv_line children of the chart, since
     * lv_chart's own line drawing ignores dash style props. Mapped into
     * the chart's own content box (padding-inset), matching exactly how
     * lv_chart itself positions series points (lv_chart_get_point_pos_by_id())
     * - otherwise the dashed target line spans the full raw box while the
     * actual solid series stops short of both edges, making the target
     * line look like it overflows past the real plot area.
     *
     * IMPORTANT: lv_obj_move_to() already offsets a child's local (0,0) by
     * the PARENT's pad_left/pad_top (see lv_obj_pos.c) since these lines
     * are positioned via lv_obj_set_pos(line, 0, 0) - so the point math
     * below must stay in content-box-LOCAL space (0..content_w/content_h)
     * and must NOT add pad_left/pad_top again, or the inset gets applied
     * twice (large gap on the left, overflow on the right). */
    if (has_profile && chart_w > 0 && chart_h > 0) {
        static lv_point_t temp_pts[MAX_CHART_POINTS];
        static lv_point_t fan_pts[MAX_CHART_POINTS];

        lv_coord_t content_w = lv_obj_get_content_width(chart);
        lv_coord_t content_h = lv_obj_get_content_height(chart);

        for (int i = 0; i < MAX_CHART_POINTS; i++) {
            uint32_t t_s = (uint32_t)((uint64_t)i * duration_s / (MAX_CHART_POINTS - 1));
            float target_temp = roast_profile_get_target_temp_c(&meta.profile, t_s);
            uint8_t target_fan = roast_profile_get_target_fan_pct(&meta.profile, t_s);

            lv_coord_t x = (lv_coord_t)((uint64_t)i * content_w / (MAX_CHART_POINTS - 1));

            float temp_frac = target_temp / 260.0f;
            if (temp_frac < 0.0f) temp_frac = 0.0f;
            if (temp_frac > 1.0f) temp_frac = 1.0f;
            temp_pts[i].x = x;
            temp_pts[i].y = content_h - (lv_coord_t)(temp_frac * content_h);

            float fan_frac = (float)target_fan / 100.0f;
            if (fan_frac < 0.0f) fan_frac = 0.0f;
            if (fan_frac > 1.0f) fan_frac = 1.0f;
            fan_pts[i].x = x;
            fan_pts[i].y = content_h - (lv_coord_t)(fan_frac * content_h);
        }

        lv_obj_t *target_temp_line = lv_line_create(chart);
        lv_obj_set_style_line_color(target_temp_line, lv_color_hex(0xFF9746), LV_PART_MAIN);
        lv_obj_set_style_line_width(target_temp_line, 2, LV_PART_MAIN);
        lv_obj_set_style_line_dash_width(target_temp_line, 5, LV_PART_MAIN);
        lv_obj_set_style_line_dash_gap(target_temp_line, 5, LV_PART_MAIN);
        lv_obj_clear_flag(target_temp_line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(target_temp_line, 0, 0);
        lv_obj_set_size(target_temp_line, content_w, content_h);
        lv_line_set_points(target_temp_line, temp_pts, MAX_CHART_POINTS);

        lv_obj_t *target_fan_line = lv_line_create(chart);
        lv_obj_set_style_line_color(target_fan_line, lv_color_hex(0x66BB6A), LV_PART_MAIN);
        lv_obj_set_style_line_width(target_fan_line, 2, LV_PART_MAIN);
        lv_obj_set_style_line_dash_width(target_fan_line, 5, LV_PART_MAIN);
        lv_obj_set_style_line_dash_gap(target_fan_line, 5, LV_PART_MAIN);
        lv_obj_clear_flag(target_fan_line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(target_fan_line, 0, 0);
        lv_obj_set_size(target_fan_line, content_w, content_h);
        lv_line_set_points(target_fan_line, fan_pts, MAX_CHART_POINTS);

        /* Numeric value labels at each segment's own setpoint (operator
         * request: the chart had no numeric markings at all) - one small
         * label per segment for temp (orange) and fan (green), centered
         * over that segment's flat step. This whole screen is rebuilt
         * fresh every time it's shown, so nothing to clean up beforehand. */
        uint32_t seg_cursor_s = 0;
        for (uint8_t i = 0; i < meta.profile.point_count; i++) {
            const roast_profile_point_t *pt = &meta.profile.points[i];
            uint32_t seg_mid_s = seg_cursor_s + pt->duration_s / 2;
            seg_cursor_s += pt->duration_s;

            lv_coord_t x = (lv_coord_t)((uint64_t)seg_mid_s * content_w / (duration_s > 0 ? duration_s : 1));
            if (x < 0) x = 0;
            if (x > content_w) x = content_w;

            float temp_frac = pt->target_temp_c / 260.0f;
            if (temp_frac < 0.0f) temp_frac = 0.0f;
            if (temp_frac > 1.0f) temp_frac = 1.0f;
            lv_coord_t temp_y = content_h - (lv_coord_t)(temp_frac * content_h);

            char seg_buf[8];
            lv_obj_t *temp_lbl = lv_label_create(chart);
            lv_obj_add_style(temp_lbl, &s_style_seg_temp_label, LV_PART_MAIN);
            snprintf(seg_buf, sizeof(seg_buf), "%.0f", (double)pt->target_temp_c);
            lv_label_set_text(temp_lbl, seg_buf);
            lv_obj_set_pos(temp_lbl, x - 8, temp_y - 16);

            float fan_frac = (float)pt->target_fan_pct / 100.0f;
            if (fan_frac < 0.0f) fan_frac = 0.0f;
            if (fan_frac > 1.0f) fan_frac = 1.0f;
            lv_coord_t fan_y = content_h - (lv_coord_t)(fan_frac * content_h);

            lv_obj_t *fan_lbl = lv_label_create(chart);
            lv_obj_add_style(fan_lbl, &s_style_seg_fan_label, LV_PART_MAIN);
            snprintf(seg_buf, sizeof(seg_buf), "%u%%", (unsigned)pt->target_fan_pct);
            lv_label_set_text(fan_lbl, seg_buf);
            lv_obj_set_pos(fan_lbl, x - 8, fan_y + 4);
        }
    }

    /* Compact single-line legend, short enough to never need ellipsis at
     * this width. */
    lv_obj_t *legend = lv_label_create(root);
    lv_obj_add_style(legend, &s_style_stat_label, LV_PART_MAIN);
    lv_label_set_text(legend, "Dashed = target, solid = actual (BT orange / Fan green)");
    lv_label_set_long_mode(legend, LV_LABEL_LONG_DOT);
    lv_obj_set_width(legend, content_w);

    /* Stats panel: two flex rows of two labels each (same row/space-between
     * pattern as the rest of the layout), whatever's meaningfully derivable
     * from the recorded telemetry (no event markers exist yet - T056 - so
     * charge/FC/drop-specific stats like Artisan's aren't available yet;
     * these are the honest subset that is). */
    int64_t total_s = max_t_ms / 1000;
    long avg_heater = (sample_count > 0) ? (heater_sum / (long)sample_count) : 0;
    long avg_fan = (sample_count > 0) ? (fan_sum / (long)sample_count) : 0;
    char stat_buf[64];

    lv_obj_t *stat_row1 = make_flex_row(root, content_w);
    lv_obj_t *stat1 = lv_label_create(stat_row1);
    lv_obj_add_style(stat1, &s_style_stat_label, LV_PART_MAIN);
    snprintf(stat_buf, sizeof(stat_buf), "Total time: %02d:%02d", (int)(total_s / 60), (int)(total_s % 60));
    lv_label_set_text(stat1, stat_buf);
    lv_obj_t *stat2 = lv_label_create(stat_row1);
    lv_obj_add_style(stat2, &s_style_stat_label, LV_PART_MAIN);
    snprintf(stat_buf, sizeof(stat_buf), "Max BT: %.1f C", max_bt);
    lv_label_set_text(stat2, stat_buf);

    lv_obj_t *stat_row2 = make_flex_row(root, content_w);
    lv_obj_t *stat3 = lv_label_create(stat_row2);
    lv_obj_add_style(stat3, &s_style_stat_label, LV_PART_MAIN);
    snprintf(stat_buf, sizeof(stat_buf), "Max RoR: %.1f C/min", max_ror);
    lv_label_set_text(stat3, stat_buf);
    lv_obj_t *stat4 = lv_label_create(stat_row2);
    lv_obj_add_style(stat4, &s_style_stat_label, LV_PART_MAIN);
    snprintf(stat_buf, sizeof(stat_buf), "Avg Fan/Heater: %ld%%/%ld%%", avg_fan, avg_heater);
    lv_label_set_text(stat4, stat_buf);
}

static void session_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const char *session_id = (const char *)lv_event_get_user_data(e);
    show_session_detail(s_content_parent, session_id);
}

/* Auto-disarms the "Delete All" confirmation after a few seconds if the
 * operator doesn't tap again - avoids a stray second tap long after the
 * fact accidentally wiping history. */
static void disarm_delete_all_cb(lv_timer_t *timer)
{
    (void)timer;
    s_delete_all_armed = false;
    s_disarm_timer = NULL;
    if (s_delete_all_lbl != NULL) {
        lv_label_set_text(s_delete_all_lbl, LV_SYMBOL_TRASH " Delete All");
    }
}

static void delete_all_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_delete_all_armed) {
        /* First tap: arm a confirmation window instead of deleting
         * immediately - this is a destructive, irreversible action. */
        s_delete_all_armed = true;
        lv_label_set_text(s_delete_all_lbl, "Confirm?");
        if (s_disarm_timer != NULL) {
            lv_timer_del(s_disarm_timer);
        }
        s_disarm_timer = lv_timer_create(disarm_delete_all_cb, 4000, NULL);
        lv_timer_set_repeat_count(s_disarm_timer, 1);
        return;
    }

    if (s_disarm_timer != NULL) {
        lv_timer_del(s_disarm_timer);
        s_disarm_timer = NULL;
    }
    s_delete_all_armed = false;
    session_store_delete_all();
    lv_obj_clean(s_content_parent);
    show_list(s_content_parent);
}

static void show_list(lv_obj_t *parent)
{
    ensure_styles();

    /* Reset delete-all confirmation state on every (re)build of this
     * screen so it never carries an armed/stale state across navigation. */
    s_delete_all_armed = false;
    if (s_disarm_timer != NULL) {
        lv_timer_del(s_disarm_timer);
        s_disarm_timer = NULL;
    }

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(title, "Roast History");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    s_delete_all_btn = lv_btn_create(parent);
    lv_obj_add_style(s_delete_all_btn, &s_style_btn_danger, LV_PART_MAIN);
    lv_obj_set_size(s_delete_all_btn, 120, 26);
    lv_obj_align(s_delete_all_btn, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_event_cb(s_delete_all_btn, delete_all_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_delete_all_lbl = lv_label_create(s_delete_all_btn);
    lv_obj_add_style(s_delete_all_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(s_delete_all_lbl, LV_SYMBOL_TRASH " Delete All");
    lv_obj_center(s_delete_all_lbl);

    static char session_ids[MAX_SESSIONS_LISTED][SESSION_STORE_ID_MAX_LEN];
    size_t count = 0;
    session_store_list_sessions(session_ids, MAX_SESSIONS_LISTED, &count);

    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, lv_obj_get_width(parent) - 16, lv_obj_get_height(parent) - 48);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);

    if (count == 0) {
        lv_list_add_text(list, "No completed roasts yet");
    } else {
        for (size_t i = 0; i < count; i++) {
            char display_name[80];
            format_session_display_name(session_ids[i], display_name, sizeof(display_name));
            lv_obj_t *btn = lv_list_add_btn(list, NULL, display_name);
            lv_obj_add_event_cb(btn, session_item_event_cb, LV_EVENT_CLICKED, session_ids[i]);
        }
    }

    ESP_LOGI(TAG, "Roast history shown (%d sessions)", (int)count);
}

void session_review_show_in(lv_obj_t *parent)
{
    s_content_parent = parent;
    show_list(parent);
}

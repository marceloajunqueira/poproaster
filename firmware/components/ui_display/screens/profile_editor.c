/**
 * @file profile_editor.c
 * @brief See header.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "storage/profile_store.h"
#include "ui_display/screens/profile_editor.h"
#include "ui_display/screens/profile_list.h"

static const char *TAG = "profile_editor";

/* Duration steps in whole 15s increments, clamped to a sane [15s, 30min]
 * per-segment range; temp/fan/heater step by 5 within their natural ranges. */
#define DURATION_STEP_S 15
#define DURATION_MIN_S 15
#define DURATION_MAX_S 1800
#define TEMP_STEP_C 5.0f
#define PCT_STEP 5

typedef enum {
    FIELD_DURATION,
    FIELD_TEMP,
    FIELD_FAN,
} field_t;

typedef struct {
    uint8_t seg_idx;
    field_t field;
    int16_t delta;
} stepper_action_t;

/* Stable storage for stepper button user_data pointers - rebuilt (count
 * reset to 0) at the start of every rebuild_editor_ui() call; safe because
 * the buttons referencing old entries are always deleted (lv_obj_clean)
 * before the array is reused. */
#define MAX_ACTIONS (ROAST_PROFILE_MAX_POINTS * 4 * 2 + 8)
static stepper_action_t s_actions[MAX_ACTIONS];
static int s_action_count;

static lv_obj_t *s_parent;
static roast_profile_t s_working;
static int s_editing_id;

static lv_style_t s_style_label;
static lv_style_t s_style_stat_label;
static lv_style_t s_style_btn_primary;
static lv_style_t s_style_btn_secondary;
static lv_style_t s_style_btn_danger;
static lv_style_t s_style_btn_label;
static bool s_styles_ready = false;

static void rebuild_editor_ui(void);

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, lv_color_hex(0xe0e0e0));

    lv_style_init(&s_style_stat_label);
    lv_style_set_text_color(&s_style_stat_label, lv_color_hex(0x9e9e9e));

    lv_style_init(&s_style_btn_primary);
    lv_style_set_bg_color(&s_style_btn_primary, lv_color_hex(0xFF9746));
    lv_style_set_bg_opa(&s_style_btn_primary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_primary, 6);

    lv_style_init(&s_style_btn_secondary);
    lv_style_set_bg_color(&s_style_btn_secondary, lv_color_hex(0x616161));
    lv_style_set_bg_opa(&s_style_btn_secondary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_secondary, 6);

    lv_style_init(&s_style_btn_danger);
    lv_style_set_bg_color(&s_style_btn_danger, lv_color_hex(0xB3261E));
    lv_style_set_bg_opa(&s_style_btn_danger, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_danger, 6);

    lv_style_init(&s_style_btn_label);
    lv_style_set_text_color(&s_style_btn_label, lv_color_hex(0xFFFFFF));

    s_styles_ready = true;
}

/* Same transparent-full-width-flex-row helper duplicated in every screen
 * that needs it (roast_dashboard.c, session_review.c) - rows stack
 * automatically via the parent's flex layout instead of manual pixel
 * y-coordinates. */
static lv_obj_t *make_row(lv_obj_t *col_parent, lv_coord_t width)
{
    lv_obj_t *row = lv_obj_create(col_parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* SPACE_BETWEEN only distributes LEFTOVER space between children - when
     * a middle child (e.g. the name field, textarea) has flex_grow=1 and
     * consumes all of it, adjacent fixed-size buttons end up touching it
     * with zero gap. pad_column enforces a minimum gap regardless. */
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    return row;
}

static stepper_action_t *alloc_action(uint8_t seg_idx, field_t field, int16_t delta)
{
    if (s_action_count >= MAX_ACTIONS) {
        s_action_count = MAX_ACTIONS - 1; /* Defensive - shouldn't happen given the sizing above. */
    }
    stepper_action_t *a = &s_actions[s_action_count++];
    a->seg_idx = seg_idx;
    a->field = field;
    a->delta = delta;
    return a;
}

static void go_back_to_list(void)
{
    lv_obj_clean(s_parent);
    profile_list_show_in(s_parent);
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    go_back_to_list();
}

static void save_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    esp_err_t err;
    if (s_editing_id < 0) {
        int new_id = -1;
        err = profile_store_create(&s_working, &new_id);
    } else {
        err = profile_store_update(s_editing_id, &s_working);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save profile '%s': %s", s_working.name, esp_err_to_name(err));
        return; /* Stay on the editor so the operator doesn't lose their changes. */
    }
    go_back_to_list();
}

static void delete_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_editing_id >= 0) {
        profile_store_delete(s_editing_id);
    }
    go_back_to_list();
}

static void add_segment_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_working.point_count >= ROAST_PROFILE_MAX_POINTS) {
        ESP_LOGW(TAG, "Cannot add another segment - already at the max (%d)", ROAST_PROFILE_MAX_POINTS);
        return;
    }
    /* Clone the last segment's values as a reasonable starting point for the new one. */
    roast_profile_point_t new_pt = { .duration_s = 60, .target_temp_c = 200.0f, .target_fan_pct = 60, .is_cooling = false };
    if (s_working.point_count > 0) {
        new_pt = s_working.points[s_working.point_count - 1];
    }
    s_working.points[s_working.point_count] = new_pt;
    s_working.point_count++;
    rebuild_editor_ui();
}

static void delete_segment_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_working.point_count <= 1) {
        ESP_LOGW(TAG, "Cannot delete the last remaining segment - a profile needs at least one");
        return;
    }
    uint8_t idx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    for (uint8_t i = idx; i < s_working.point_count - 1; i++) {
        s_working.points[i] = s_working.points[i + 1];
    }
    s_working.point_count--;
    rebuild_editor_ui();
}

static void toggle_cooling_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uint8_t idx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    roast_profile_point_t *pt = &s_working.points[idx];
    pt->is_cooling = !pt->is_cooling;
    if (pt->is_cooling) {
        /* Cooling always uses these two fixed values (not operator-editable -
         * see roast_profile.h) - temperature target is irrelevant once the
         * heater is forced off, and full fan speed cools fastest/safest. */
        pt->target_temp_c = ROAST_PROFILE_COOLING_TEMP_C;
        pt->target_fan_pct = ROAST_PROFILE_COOLING_FAN_PCT;
    }
    rebuild_editor_ui();
}

static void stepper_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    stepper_action_t *act = (stepper_action_t *)lv_event_get_user_data(e);
    roast_profile_point_t *pt = &s_working.points[act->seg_idx];

    switch (act->field) {
    case FIELD_DURATION: {
        int32_t v = (int32_t)pt->duration_s + act->delta;
        if (v < DURATION_MIN_S) v = DURATION_MIN_S;
        if (v > DURATION_MAX_S) v = DURATION_MAX_S;
        pt->duration_s = (uint32_t)v;
        break;
    }
    case FIELD_TEMP: {
        float v = pt->target_temp_c + (float)act->delta;
        if (v < 0.0f) v = 0.0f;
        if (v > 260.0f) v = 260.0f;
        pt->target_temp_c = v;
        break;
    }
    case FIELD_FAN: {
        int v = (int)pt->target_fan_pct + act->delta;
        /* FR-004: normal segments must keep fan at/above the fixed floor -
         * Cooling segments never reach this stepper at all (its fan is
         * fixed at ROAST_PROFILE_COOLING_FAN_PCT, see build_segment_card()). */
        if (v < ROAST_PROFILE_FAN_MIN_PCT) v = ROAST_PROFILE_FAN_MIN_PCT;
        if (v > 100) v = 100;
        pt->target_fan_pct = (uint8_t)v;
        break;
    }
    }
    rebuild_editor_ui();
}

static void rename_ok_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(ta);
    if (text != NULL && text[0] != '\0') {
        strncpy(s_working.name, text, sizeof(s_working.name) - 1);
        s_working.name[sizeof(s_working.name) - 1] = '\0';
    }
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(ta));
    lv_obj_del(overlay);
    rebuild_editor_ui();
}

static void rename_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_del(overlay);
}

static void rename_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    /* Full-screen overlay (parented directly to the screen, like nav_shell's
     * alarm banner, so it covers the sidebar too) with a textarea + on-screen
     * keyboard for renaming the profile. */
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x121212), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(overlay);

    lv_obj_t *row = lv_obj_create(overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(90), 40);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Same SPACE_BETWEEN + flex_grow gap issue as make_row() above - the
     * textarea's flex_grow=1 eats all the leftover space, leaving zero gap
     * to the OK/Cancel buttons without an explicit minimum column gap. */
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, ROAST_PROFILE_NAME_MAX_LEN - 1);
    lv_textarea_set_text(ta, s_working.name);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_height(ta, 36);

    lv_obj_t *ok_btn = lv_btn_create(row);
    lv_obj_add_style(ok_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(ok_btn, 50, 32);
    lv_obj_add_event_cb(ok_btn, rename_ok_cb, LV_EVENT_CLICKED, ta);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_obj_add_style(ok_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(ok_lbl, LV_SYMBOL_OK);
    lv_obj_center(ok_lbl);

    lv_obj_t *cancel_btn = lv_btn_create(row);
    lv_obj_add_style(cancel_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(cancel_btn, 50, 32);
    lv_obj_add_event_cb(cancel_btn, rename_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_obj_add_style(cancel_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(cancel_lbl);

    lv_obj_t *kb = lv_keyboard_create(overlay);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), 170);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

static void make_stepper(lv_obj_t *parent_row, const char *value_str, uint8_t seg_idx, field_t field, int16_t minus_delta, int16_t plus_delta)
{
    lv_obj_t *group = lv_obj_create(parent_row);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, 108, 28);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *minus_btn = lv_btn_create(group);
    lv_obj_add_style(minus_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(minus_btn, 26, 26);
    lv_obj_add_event_cb(minus_btn, stepper_event_cb, LV_EVENT_CLICKED, alloc_action(seg_idx, field, minus_delta));
    lv_obj_t *minus_lbl = lv_label_create(minus_btn);
    lv_obj_add_style(minus_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(minus_lbl);

    lv_obj_t *value_lbl = lv_label_create(group);
    lv_obj_add_style(value_lbl, &s_style_stat_label, LV_PART_MAIN);
    lv_obj_set_width(value_lbl, 46);
    lv_obj_set_style_text_align(value_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(value_lbl, value_str);

    lv_obj_t *plus_btn = lv_btn_create(group);
    lv_obj_add_style(plus_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(plus_btn, 26, 26);
    lv_obj_add_event_cb(plus_btn, stepper_event_cb, LV_EVENT_CLICKED, alloc_action(seg_idx, field, plus_delta));
    lv_obj_t *plus_lbl = lv_label_create(plus_btn);
    lv_obj_add_style(plus_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(plus_lbl);
}

static void build_segment_card(lv_obj_t *parent, uint8_t idx, lv_coord_t width)
{
    const roast_profile_point_t *pt = &s_working.points[idx];

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1e1e1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    lv_coord_t inner_w = width - 12;

    lv_obj_t *row_a = make_row(card, inner_w);
    lv_obj_t *title_lbl = lv_label_create(row_a);
    lv_obj_add_style(title_lbl, &s_style_label, LV_PART_MAIN);
    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Seg %d%s", idx + 1, pt->is_cooling ? " (Cooling)" : "");
    lv_label_set_text(title_lbl, title_buf);

    lv_obj_t *cool_btn = lv_btn_create(row_a);
    lv_obj_add_style(cool_btn, pt->is_cooling ? &s_style_btn_primary : &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(cool_btn, 76, 22);
    lv_obj_add_event_cb(cool_btn, toggle_cooling_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *cool_lbl = lv_label_create(cool_btn);
    lv_obj_add_style(cool_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(cool_lbl, pt->is_cooling ? "Cooling" : "Heating");
    lv_obj_center(cool_lbl);

    lv_obj_t *del_btn = lv_btn_create(row_a);
    lv_obj_add_style(del_btn, &s_style_btn_danger, LV_PART_MAIN);
    lv_obj_set_size(del_btn, 26, 22);
    lv_obj_add_event_cb(del_btn, delete_segment_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_obj_add_style(del_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
    lv_obj_center(del_lbl);

    lv_obj_t *row_b = lv_obj_create(card);
    lv_obj_remove_style_all(row_b);
    lv_obj_set_size(row_b, inner_w, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row_b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_b, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row_b, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(row_b, 4, LV_PART_MAIN);

    char buf[16];
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(pt->duration_s / 60), (unsigned)(pt->duration_s % 60));
    make_stepper(row_b, buf, idx, FIELD_DURATION, -DURATION_STEP_S, DURATION_STEP_S);

    if (pt->is_cooling) {
        /* Cooling: only the duration is a real choice - temperature and fan
         * are fixed (see roast_profile.h), so just show them as plain
         * (non-editable) reference text instead of steppers. */
        lv_obj_t *fixed_lbl = lv_label_create(row_b);
        lv_obj_add_style(fixed_lbl, &s_style_stat_label, LV_PART_MAIN);
        lv_label_set_text(fixed_lbl, "Fixed: 0C, 100% Fan");
    } else {
        snprintf(buf, sizeof(buf), "%.0fC", pt->target_temp_c);
        make_stepper(row_b, buf, idx, FIELD_TEMP, -(int16_t)TEMP_STEP_C, (int16_t)TEMP_STEP_C);

        snprintf(buf, sizeof(buf), "%u%%F", (unsigned)pt->target_fan_pct);
        make_stepper(row_b, buf, idx, FIELD_FAN, -PCT_STEP, PCT_STEP);
    }
}

static void rebuild_editor_ui(void)
{
    ensure_styles();
    lv_obj_clean(s_parent);
    s_action_count = 0;

    lv_coord_t content_w = lv_obj_get_width(s_parent);
    lv_coord_t content_h = lv_obj_get_height(s_parent);
    lv_coord_t row_w = content_w - 16;

    lv_obj_t *root = lv_obj_create(s_parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, content_w, content_h);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 6, LV_PART_MAIN);

    /* Header: Back, profile name (tap to rename), Save. */
    lv_obj_t *header = make_row(root, row_w);
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_add_style(back_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(back_btn, 60, 26);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_obj_add_style(back_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    lv_obj_t *name_btn = lv_btn_create(header);
    lv_obj_add_style(name_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_flex_grow(name_btn, 1);
    lv_obj_set_height(name_btn, 26);
    lv_obj_add_event_cb(name_btn, rename_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *name_lbl = lv_label_create(name_btn);
    lv_obj_add_style(name_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(name_lbl, s_working.name);
    lv_obj_center(name_lbl);

    lv_obj_t *save_btn = lv_btn_create(header);
    lv_obj_add_style(save_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(save_btn, 60, 26);
    lv_obj_add_event_cb(save_btn, save_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_obj_add_style(save_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(save_lbl);

    /* Scrollable segment list - grows to fill whatever space is left. */
    lv_obj_t *seg_area = lv_obj_create(root);
    lv_obj_remove_style_all(seg_area);
    lv_obj_set_width(seg_area, LV_PCT(100));
    lv_obj_set_flex_grow(seg_area, 1);
    lv_obj_set_flex_flow(seg_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seg_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(seg_area, 6, LV_PART_MAIN);

    for (uint8_t i = 0; i < s_working.point_count; i++) {
        build_segment_card(seg_area, i, row_w);
    }

    /* Footer: Add Segment (+ Delete Profile, only for an existing one). */
    lv_obj_t *footer = make_row(root, row_w);
    lv_obj_t *add_btn = lv_btn_create(footer);
    lv_obj_add_style(add_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(add_btn, 130, 28);
    lv_obj_add_event_cb(add_btn, add_segment_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_obj_add_style(add_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add Segment");
    lv_obj_center(add_lbl);

    if (s_editing_id >= 0) {
        lv_obj_t *del_btn = lv_btn_create(footer);
        lv_obj_add_style(del_btn, &s_style_btn_danger, LV_PART_MAIN);
        lv_obj_set_size(del_btn, 130, 28);
        lv_obj_add_event_cb(del_btn, delete_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_obj_add_style(del_lbl, &s_style_btn_label, LV_PART_MAIN);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH " Delete Profile");
        lv_obj_center(del_lbl);
    }
}

void profile_editor_show_in(lv_obj_t *parent, int profile_id)
{
    s_parent = parent;
    s_editing_id = profile_id;

    if (profile_id < 0 || profile_store_load(profile_id, &s_working) != ESP_OK) {
        s_editing_id = -1;
        memset(&s_working, 0, sizeof(s_working));
        strncpy(s_working.name, "New Profile", sizeof(s_working.name) - 1);
        s_working.point_count = 1;
        s_working.points[0] = (roast_profile_point_t){
            .duration_s = 60, .target_temp_c = 200.0f, .target_fan_pct = 60, .is_cooling = false
        };
    }

    ESP_LOGI(TAG, "Profile editor shown (id=%d, name='%s')", s_editing_id, s_working.name);
    rebuild_editor_ui();
}

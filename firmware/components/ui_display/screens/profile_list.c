/**
 * @file profile_list.c
 * @brief See header.
 */
#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"

#include "storage/profile_store.h"
#include "ui_display/screens/profile_list.h"
#include "ui_display/screens/profile_editor.h"

static const char *TAG = "profile_list";

#define MAX_PROFILES_LISTED PROFILE_STORE_MAX_PROFILES

static lv_obj_t *s_content_parent;
static int s_armed_delete_id = -1;
static lv_obj_t *s_armed_delete_lbl;
static lv_timer_t *s_delete_disarm_timer;
static lv_obj_t *s_title_lbl;
static lv_timer_t *s_title_revert_timer;

#define PRESETS_TITLE_TEXT "Presets - tap to select, Edit to modify"

static lv_style_t s_style_label;
static lv_style_t s_style_selected_label;
static lv_style_t s_style_btn_primary;
static lv_style_t s_style_btn_secondary;
static lv_style_t s_style_btn_danger;
static lv_style_t s_style_btn_label;
static bool s_styles_ready = false;

static void show_list(lv_obj_t *parent);
static void revert_title_cb(lv_timer_t *timer);

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, lv_color_hex(0xe0e0e0));

    lv_style_init(&s_style_selected_label);
    lv_style_set_text_color(&s_style_selected_label, lv_color_hex(0xFF9746));

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
 * that needs it (roast_dashboard.c, session_review.c, profile_editor.c). */
static lv_obj_t *make_row(lv_obj_t *col_parent, lv_coord_t width)
{
    lv_obj_t *row = lv_obj_create(col_parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* SPACE_BETWEEN only distributes LEFTOVER space between children - when
     * one child (e.g. the name label) has flex_grow=1 and consumes all of
     * it, adjacent fixed-size buttons (Edit/Delete) end up with zero gap
     * between them. pad_column enforces a minimum gap regardless. */
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    return row;
}

static void select_profile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    esp_err_t err = profile_store_set_selected(id);
    if (err != ESP_OK) {
        /* Bug fix: switching presets mid-roast used to break the whole
         * screen - profile_store now refuses the switch outright while a
         * session is active. Give the operator a clear, temporary reason
         * instead of silently doing nothing. */
        if (s_title_lbl != NULL) {
            lv_label_set_text(s_title_lbl, "Cannot switch preset - a roast is active! Cancel it first.");
        }
        if (s_title_revert_timer != NULL) {
            lv_timer_del(s_title_revert_timer);
        }
        s_title_revert_timer = lv_timer_create(revert_title_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_title_revert_timer, 1);
        return;
    }
    lv_obj_clean(s_content_parent);
    show_list(s_content_parent);
}

static void edit_profile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_clean(s_content_parent);
    profile_editor_show_in(s_content_parent, id);
}

static void new_profile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_clean(s_content_parent);
    profile_editor_show_in(s_content_parent, -1);
}

/* Auto-disarms a "Delete" confirmation after a few seconds if the operator
 * doesn't tap again - avoids a stray later tap accidentally deleting a
 * preset. */
static void disarm_delete_cb(lv_timer_t *timer)
{
    (void)timer;
    s_armed_delete_id = -1;
    if (s_armed_delete_lbl != NULL) {
        lv_label_set_text(s_armed_delete_lbl, LV_SYMBOL_TRASH);
        s_armed_delete_lbl = NULL;
    }
    s_delete_disarm_timer = NULL;
}

static void revert_title_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_title_lbl != NULL) {
        lv_label_set_text(s_title_lbl, PRESETS_TITLE_TEXT);
    }
    s_title_revert_timer = NULL;
}

/*
 * Lets a preset be removed directly from the list, WITHOUT ever opening the
 * editor screen - important because a corrupted/old-format stored profile
 * can crash the editor (see profile_store_load()'s point_count clamp fix),
 * so operators need a way to delete a broken preset that never involves
 * opening it. Two-tap confirm (like Roast History's "Delete All"): first
 * tap arms this row (label -> "?") for 4s, second tap while armed deletes.
 */
static void delete_profile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);

    if (s_armed_delete_id == id) {
        if (s_delete_disarm_timer != NULL) {
            lv_timer_del(s_delete_disarm_timer);
            s_delete_disarm_timer = NULL;
        }
        s_armed_delete_id = -1;
        s_armed_delete_lbl = NULL;
        profile_store_delete(id);
        lv_obj_clean(s_content_parent);
        show_list(s_content_parent);
        return;
    }

    if (s_armed_delete_lbl != NULL) {
        lv_label_set_text(s_armed_delete_lbl, LV_SYMBOL_TRASH);
    }
    if (s_delete_disarm_timer != NULL) {
        lv_timer_del(s_delete_disarm_timer);
    }
    s_armed_delete_id = id;
    s_armed_delete_lbl = lbl;
    lv_label_set_text(lbl, "?");
    s_delete_disarm_timer = lv_timer_create(disarm_delete_cb, 4000, NULL);
    lv_timer_set_repeat_count(s_delete_disarm_timer, 1);
}

static void show_list(lv_obj_t *parent)
{
    ensure_styles();

    /* Reset delete-confirmation state on every (re)build of this screen so
     * it never carries an armed/stale state across navigation. */
    s_armed_delete_id = -1;
    s_armed_delete_lbl = NULL;
    if (s_delete_disarm_timer != NULL) {
        lv_timer_del(s_delete_disarm_timer);
        s_delete_disarm_timer = NULL;
    }
    if (s_title_revert_timer != NULL) {
        lv_timer_del(s_title_revert_timer);
        s_title_revert_timer = NULL;
    }

    lv_coord_t content_w = lv_obj_get_width(parent);
    lv_coord_t content_h = lv_obj_get_height(parent);
    lv_coord_t row_w = content_w - 16;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, content_w, content_h);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 6, LV_PART_MAIN);

    lv_obj_t *header = make_row(root, row_w);
    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &s_style_label, LV_PART_MAIN);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, PRESETS_TITLE_TEXT);
    s_title_lbl = title;

    lv_obj_t *new_btn = lv_btn_create(header);
    lv_obj_add_style(new_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(new_btn, 70, 26);
    lv_obj_add_event_cb(new_btn, new_profile_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_obj_add_style(new_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(new_lbl, LV_SYMBOL_PLUS " New");
    lv_obj_center(new_lbl);

    profile_store_entry_t entries[MAX_PROFILES_LISTED];
    size_t count = 0;
    profile_store_list(entries, MAX_PROFILES_LISTED, &count);

    int selected_id = -1;
    profile_store_get_selected_id(&selected_id);

    lv_obj_t *list_area = lv_obj_create(root);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_width(list_area, LV_PCT(100));
    lv_obj_set_flex_grow(list_area, 1);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_area, 6, LV_PART_MAIN);

    if (count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(list_area);
        lv_obj_add_style(empty_lbl, &s_style_label, LV_PART_MAIN);
        lv_label_set_text(empty_lbl, "No presets yet - tap + New to create one");
    } else {
        for (size_t i = 0; i < count; i++) {
            bool is_selected = (entries[i].id == selected_id);

            lv_obj_t *row = make_row(list_area, row_w);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, select_profile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)entries[i].id);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1e1e1e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);

            char label_buf[48];
            snprintf(label_buf, sizeof(label_buf), "%s%s", entries[i].name, is_selected ? "  (selected)" : "");
            lv_obj_t *name_lbl = lv_label_create(row);
            lv_obj_add_style(name_lbl, is_selected ? &s_style_selected_label : &s_style_label, LV_PART_MAIN);
            lv_obj_set_flex_grow(name_lbl, 1);
            lv_label_set_text(name_lbl, label_buf);

            lv_obj_t *edit_btn = lv_btn_create(row);
            lv_obj_add_style(edit_btn, &s_style_btn_secondary, LV_PART_MAIN);
            lv_obj_set_size(edit_btn, 44, 26);
            lv_obj_add_event_cb(edit_btn, edit_profile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)entries[i].id);
            lv_obj_t *edit_lbl = lv_label_create(edit_btn);
            lv_obj_add_style(edit_lbl, &s_style_btn_label, LV_PART_MAIN);
            lv_label_set_text(edit_lbl, LV_SYMBOL_EDIT);
            lv_obj_center(edit_lbl);

            lv_obj_t *delete_btn = lv_btn_create(row);
            lv_obj_add_style(delete_btn, &s_style_btn_danger, LV_PART_MAIN);
            lv_obj_set_size(delete_btn, 34, 26);
            lv_obj_add_event_cb(delete_btn, delete_profile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)entries[i].id);
            lv_obj_t *delete_lbl = lv_label_create(delete_btn);
            lv_obj_add_style(delete_lbl, &s_style_btn_label, LV_PART_MAIN);
            lv_label_set_text(delete_lbl, LV_SYMBOL_TRASH);
            lv_obj_center(delete_lbl);
        }
    }

    ESP_LOGI(TAG, "Profile list shown (%d profiles, selected id=%d)", (int)count, selected_id);
}

void profile_list_show_in(lv_obj_t *parent)
{
    s_content_parent = parent;
    show_list(parent);
}


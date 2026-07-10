/**
 * @file settings_hub.c
 * @brief See header.
 */
#include <stdio.h>
#include "esp_log.h"

#include "ui_display/i18n.h"
#include "ui_display/widgets/nav_shell.h"
#include "ui_display/screens/peripheral_test.h"
#include "ui_display/screens/sensor_calibration.h"
#include "ui_display/screens/wifi_setup.h"
#include "ui_display/screens/settings_hub.h"

static const char *TAG = "settings_hub";

static lv_obj_t *s_lang_btn_label;

static lv_style_t s_style_title;
static lv_style_t s_style_label;
static bool s_styles_ready = false;

static void build_menu(lv_obj_t *parent);

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

static void peripheral_test_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_clean(parent);
    peripheral_test_show_in(parent);
}

static void sensor_calibration_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_clean(parent);
    sensor_calibration_show_in(parent);
}

static void wifi_setup_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_clean(parent);
    wifi_setup_show_in(parent);
}

/* T055: cycles EN -> PT -> ES -> EN, persists the choice (i18n_set_language()
 * already writes it to NVS), and refreshes every currently-visible
 * i18n-driven label - the nav sidebar (nav_shell_refresh_labels()) and this
 * screen's own button labels (rebuilt in place, not a full re-navigation) -
 * so the effect is immediate, matching quickstart.md Cenario 8. */
static void language_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    i18n_lang_t current = i18n_get_language();
    i18n_lang_t next = (i18n_lang_t)((current + 1) % 3);
    i18n_set_language(next);
    nav_shell_refresh_labels();

    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_clean(parent);
    build_menu(parent);
}

/* 2-column icon-tile grid: each tile shows a large icon symbol above a short
 * label (same "ICON\nText" two-line-label convention nav_shell.c uses for
 * its sidebar items), instead of the previous one-per-row thin/wide
 * buttons. Tile geometry is derived from the parent's own width so it
 * still fits nicely regardless of exact content-pane size. */
#define GRID_COLS 2
#define GRID_GAP 8
#define GRID_MARGIN 12
#define TILE_HEIGHT 64

static lv_obj_t *make_grid_tile(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *symbol,
                                 const char *text, lv_event_cb_t event_cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, TILE_HEIGHT);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, w - 12);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s\n%s", symbol, text);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static void build_menu(lv_obj_t *parent)
{
    ensure_styles();

    const lv_coord_t content_w = lv_obj_get_width(parent);
    const lv_coord_t col_w = (content_w - (2 * GRID_MARGIN) - GRID_GAP) / GRID_COLS;
    const lv_coord_t col_x[GRID_COLS] = {GRID_MARGIN, GRID_MARGIN + col_w + GRID_GAP};
    const lv_coord_t row_y[2] = {36, 36 + TILE_HEIGHT + GRID_GAP};

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, i18n_get(I18N_KEY_CONFIG_TITLE));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

    make_grid_tile(parent, col_x[0], row_y[0], col_w, LV_SYMBOL_REFRESH, i18n_get(I18N_KEY_PERIPHERAL_TEST),
                    peripheral_test_btn_event_cb, parent);
    make_grid_tile(parent, col_x[1], row_y[0], col_w, LV_SYMBOL_SETTINGS, i18n_get(I18N_KEY_SENSOR_CALIBRATION),
                    sensor_calibration_btn_event_cb, parent);
    make_grid_tile(parent, col_x[0], row_y[1], col_w, LV_SYMBOL_WIFI, i18n_get(I18N_KEY_WIFI_SETUP),
                    wifi_setup_btn_event_cb, parent);

    char lang_buf[40];
    snprintf(lang_buf, sizeof(lang_buf), "%s: %s", i18n_get(I18N_KEY_LANGUAGE), i18n_get_language_code(i18n_get_language()));
    lv_obj_t *lang_btn = make_grid_tile(parent, col_x[1], row_y[1], col_w, LV_SYMBOL_KEYBOARD, lang_buf,
                                         language_btn_event_cb, parent);
    s_lang_btn_label = lv_obj_get_child(lang_btn, 0);
}

void settings_hub_show_in(lv_obj_t *parent)
{
    build_menu(parent);
    ESP_LOGI(TAG, "Settings hub shown");
}

void settings_hub_return_to_menu(lv_obj_t *parent)
{
    build_menu(parent);
}

void settings_hub_hide(void)
{
    /* Safe to call both unconditionally - each is a no-op if its own
     * screen isn't the one currently active (nothing to tear down). */
    peripheral_test_hide();
    sensor_calibration_hide();
}

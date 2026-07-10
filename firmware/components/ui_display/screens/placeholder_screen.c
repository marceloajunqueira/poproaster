/**
 * @file placeholder_screen.c
 * @brief See header.
 */
#include "ui_display/screens/placeholder_screen.h"

static lv_style_t s_style_title;
static lv_style_t s_style_body;
static bool s_styles_ready = false;

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, lv_color_hex(0xe0e0e0));

    lv_style_init(&s_style_body);
    lv_style_set_text_color(&s_style_body, lv_color_hex(0x9e9e9e));

    s_styles_ready = true;
}

void placeholder_screen_show_in(lv_obj_t *parent, const char *title, const char *body)
{
    ensure_styles();

    lv_obj_t *title_lbl = lv_label_create(parent);
    lv_obj_add_style(title_lbl, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title_lbl, title);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 16, 16);

    lv_obj_t *body_lbl = lv_label_create(parent);
    lv_obj_add_style(body_lbl, &s_style_body, LV_PART_MAIN);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body_lbl, lv_obj_get_width(parent) - 32);
    lv_label_set_text(body_lbl, body);
    lv_obj_align(body_lbl, LV_ALIGN_TOP_LEFT, 16, 56);
}

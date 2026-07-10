/**
 * @file wifi_setup.c
 * @brief See header.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "hal/wifi_provisioning.h"
#include "ui_display/screens/settings_hub.h"
#include "ui_display/screens/wifi_setup.h"

static const char *TAG = "wifi_setup";

static char s_ssid[33] = {0};
static char s_password[65] = {0};

static lv_obj_t *s_status_label;
static lv_obj_t *s_ssid_value_label;
static lv_obj_t *s_password_value_label;
static lv_timer_t *s_refresh_timer;

static lv_style_t s_style_title;
static lv_style_t s_style_label;
static lv_style_t s_style_btn_primary;
static lv_style_t s_style_btn_secondary;
static lv_style_t s_style_btn_label;
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
    lv_style_init(&s_style_btn_primary);
    lv_style_set_bg_color(&s_style_btn_primary, lv_color_hex(0xFF9746));
    lv_style_set_bg_opa(&s_style_btn_primary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_primary, 6);
    lv_style_init(&s_style_btn_secondary);
    lv_style_set_bg_color(&s_style_btn_secondary, lv_color_hex(0x616161));
    lv_style_set_bg_opa(&s_style_btn_secondary, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_secondary, 6);
    lv_style_init(&s_style_btn_label);
    lv_style_set_text_color(&s_style_btn_label, lv_color_hex(0xFFFFFF));
    s_styles_ready = true;
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    char buf[64];
    wifi_prov_state_t state = wifi_provisioning_get_state();
    char ip_buf[24] = "";
    switch (state) {
    case WIFI_PROV_STATE_AP_PORTAL:
        snprintf(buf, sizeof(buf), "Status: AP mode (PopRoaster-Setup)");
        break;
    case WIFI_PROV_STATE_CONNECTING:
        snprintf(buf, sizeof(buf), "Status: Connecting...");
        break;
    case WIFI_PROV_STATE_CONNECTED:
        wifi_provisioning_get_ip_str(ip_buf, sizeof(ip_buf));
        snprintf(buf, sizeof(buf), "Status: Connected (%s)", ip_buf[0] ? ip_buf : "?");
        break;
    case WIFI_PROV_STATE_FAILED:
        snprintf(buf, sizeof(buf), "Status: Failed - back in AP mode");
        break;
    default:
        snprintf(buf, sizeof(buf), "Status: --");
        break;
    }
    lv_label_set_text(s_status_label, buf);
}

/* Generic-enough textarea+keyboard overlay (same technique as
 * profile_editor.c's rename dialog): tapping OK copies the textarea's text
 * into `dest`/`dest_label` (both passed via lv_event_get_user_data() as a
 * 2-element pointer array stashed in a static slot, since LVGL callbacks
 * only carry one void* - simplest to just special-case SSID vs password
 * with two thin wrapper callbacks instead of a generic struct). */
static char *s_edit_dest;
static size_t s_edit_dest_len;
static lv_obj_t *s_edit_preview_label;
static const char *s_edit_preview_prefix;

static void edit_ok_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(ta);
    if (text != NULL) {
        strncpy(s_edit_dest, text, s_edit_dest_len - 1);
        s_edit_dest[s_edit_dest_len - 1] = '\0';
    }
    if (s_edit_preview_label != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%s", s_edit_preview_prefix, s_edit_dest[0] ? s_edit_dest : "(empty)");
        lv_label_set_text(s_edit_preview_label, buf);
    }
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(ta));
    lv_obj_del(overlay);
}

static void edit_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_del(overlay);
}

static void open_edit_overlay(const char *title, char *dest, size_t dest_len, lv_obj_t *preview_label,
                               const char *preview_prefix, bool is_password)
{
    s_edit_dest = dest;
    s_edit_dest_len = dest_len;
    s_edit_preview_label = preview_label;
    s_edit_preview_prefix = preview_prefix;

    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x121212), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(overlay);
    lv_obj_add_style(title_lbl, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title_lbl, title);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *row = lv_obj_create(overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(90), 40);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, is_password);
    lv_textarea_set_max_length(ta, (uint32_t)(dest_len - 1));
    lv_textarea_set_text(ta, dest);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_height(ta, 36);

    lv_obj_t *ok_btn = lv_btn_create(row);
    lv_obj_add_style(ok_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(ok_btn, 50, 32);
    lv_obj_add_event_cb(ok_btn, edit_ok_cb, LV_EVENT_CLICKED, ta);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_obj_add_style(ok_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(ok_lbl, LV_SYMBOL_OK);
    lv_obj_center(ok_lbl);

    lv_obj_t *cancel_btn = lv_btn_create(row);
    lv_obj_add_style(cancel_btn, &s_style_btn_secondary, LV_PART_MAIN);
    lv_obj_set_size(cancel_btn, 50, 32);
    lv_obj_add_event_cb(cancel_btn, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
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

static void ssid_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    open_edit_overlay("SSID", s_ssid, sizeof(s_ssid), s_ssid_value_label, "SSID: ", false);
}

static void password_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    open_edit_overlay("Password", s_password, sizeof(s_password), s_password_value_label, "Password: ", true);
}

static void connect_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Connect tapped with empty SSID - ignored");
        return;
    }
    esp_err_t err = wifi_provisioning_set_credentials(s_ssid, s_password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_provisioning_set_credentials failed: %s", esp_err_to_name(err));
    }
}

static void back_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *parent = (lv_obj_t *)lv_event_get_user_data(e);
    wifi_setup_hide();
    lv_obj_clean(parent);
    settings_hub_return_to_menu(parent);
}

void wifi_setup_show_in(lv_obj_t *parent)
{
    ensure_styles();

    /* Pre-fill SSID with whatever was last saved (if any) - password is
     * intentionally NOT pre-filled/readable back (never stored in a way
     * this screen retrieves it in plaintext for display, though NVS itself
     * is not encrypted at rest - same trusted-device assumption as the web
     * side's wifi_setup_routes.c). */
    wifi_provisioning_get_ssid_str(s_ssid, sizeof(s_ssid));
    s_password[0] = '\0';

    const lv_coord_t content_w = lv_obj_get_width(parent);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &s_style_title, LV_PART_MAIN);
    lv_label_set_text(title, "Wi-Fi Setup");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, parent);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);

    s_status_label = lv_label_create(parent);
    lv_obj_add_style(s_status_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_status_label, "Status: --");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 44);

    s_ssid_value_label = lv_label_create(parent);
    lv_obj_add_style(s_ssid_value_label, &s_style_label, LV_PART_MAIN);
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "SSID: %s", s_ssid[0] ? s_ssid : "(empty)");
        lv_label_set_text(s_ssid_value_label, buf);
    }
    lv_obj_align(s_ssid_value_label, LV_ALIGN_TOP_LEFT, 12, 76);

    lv_obj_t *ssid_btn = lv_btn_create(parent);
    lv_obj_set_size(ssid_btn, content_w - 24, 30);
    lv_obj_align(ssid_btn, LV_ALIGN_TOP_LEFT, 12, 100);
    lv_obj_add_event_cb(ssid_btn, ssid_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ssid_btn_lbl = lv_label_create(ssid_btn);
    lv_label_set_text(ssid_btn_lbl, "Set SSID");
    lv_obj_center(ssid_btn_lbl);

    s_password_value_label = lv_label_create(parent);
    lv_obj_add_style(s_password_value_label, &s_style_label, LV_PART_MAIN);
    lv_label_set_text(s_password_value_label, "Password: (empty)");
    lv_obj_align(s_password_value_label, LV_ALIGN_TOP_LEFT, 12, 138);

    lv_obj_t *password_btn = lv_btn_create(parent);
    lv_obj_set_size(password_btn, content_w - 24, 30);
    lv_obj_align(password_btn, LV_ALIGN_TOP_LEFT, 12, 162);
    lv_obj_add_event_cb(password_btn, password_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *password_btn_lbl = lv_label_create(password_btn);
    lv_label_set_text(password_btn_lbl, "Set Password");
    lv_obj_center(password_btn_lbl);

    lv_obj_t *connect_btn = lv_btn_create(parent);
    lv_obj_add_style(connect_btn, &s_style_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(connect_btn, content_w - 24, 34);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_LEFT, 12, 200);
    lv_obj_add_event_cb(connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_obj_add_style(connect_lbl, &s_style_btn_label, LV_PART_MAIN);
    lv_label_set_text(connect_lbl, LV_SYMBOL_WIFI " Connect");
    lv_obj_center(connect_lbl);

    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    refresh_timer_cb(NULL);

    ESP_LOGI(TAG, "Wi-Fi setup screen shown");
}

void wifi_setup_hide(void)
{
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    s_status_label = NULL;
    s_ssid_value_label = NULL;
    s_password_value_label = NULL;
}

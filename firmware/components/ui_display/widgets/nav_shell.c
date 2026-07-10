/**
 * @file nav_shell.c
 * @brief See header.
 */
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include "roast_core/command_dispatcher.h"
#include "roast_core/session_state_machine.h"
#include "safety/safety_manager.h"
#include "ui_display/i18n.h"
#include "ui_display/widgets/nav_shell.h"

static const char *TAG = "nav_shell";

#define SIDEBAR_WIDTH 76
/* 5 tabs (Roast/Manual/Presets/History/Config) + the pinned Emergency Stop
 * button must all fit within the 272px display height: 5*43 + 56 = 271. */
#define TAB_HEIGHT 43
#define ESTOP_HEIGHT 56

#define COLOR_BG 0x121212
#define COLOR_SIDEBAR_BG 0x1E1E1E
#define COLOR_PRIMARY 0xFF9746
#define COLOR_PRIMARY_DIM 0x3A2A1D /* low-opacity-looking tint of the primary accent, for the active tab bg */
#define COLOR_TEXT_INACTIVE 0x9e9e9e
#define COLOR_TEXT_ACTIVE 0xFFFFFF
#define COLOR_DANGER 0xD32F2F

typedef struct {
    const char *label;
    nav_shell_show_fn_t show_fn;
    nav_shell_hide_fn_t hide_fn;
    lv_obj_t *item;
    lv_obj_t *item_label;
    lv_obj_t *accent_bar;
} nav_tab_entry_t;

static nav_tab_entry_t s_tabs[NAV_SHELL_TAB_COUNT];
static nav_shell_tab_t s_current_tab = NAV_SHELL_TAB_COUNT;
static lv_obj_t *s_content;
static lv_obj_t *s_sidebar;
static bool s_styles_ready = false;

/* Critical alarm banner (T024/FR-029): always-visible regardless of active
 * tab, requires an explicit "ACKNOWLEDGE" tap before it goes away, even if
 * the underlying condition already cleared itself. */
static lv_obj_t *s_alarm_banner;
static lv_obj_t *s_alarm_label;
static lv_timer_t *s_alarm_timer;

static lv_style_t s_style_screen;
static lv_style_t s_style_sidebar;
static lv_style_t s_style_item;
static lv_style_t s_style_item_label_inactive;
static lv_style_t s_style_item_label_active;
static lv_style_t s_style_accent_bar;
static lv_style_t s_style_content;
static lv_style_t s_style_estop;
static lv_style_t s_style_estop_label;
static lv_style_t s_style_alarm_banner;
static lv_style_t s_style_alarm_label;
static lv_style_t s_style_alarm_btn;
static lv_style_t s_style_alarm_btn_label;

static void ensure_styles(void)
{
    if (s_styles_ready) {
        return;
    }

    lv_style_init(&s_style_screen);
    lv_style_set_bg_color(&s_style_screen, lv_color_hex(COLOR_BG));
    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&s_style_screen, 0);
    lv_style_set_border_width(&s_style_screen, 0);

    lv_style_init(&s_style_sidebar);
    lv_style_set_bg_color(&s_style_sidebar, lv_color_hex(COLOR_SIDEBAR_BG));
    lv_style_set_bg_opa(&s_style_sidebar, LV_OPA_COVER);
    lv_style_set_radius(&s_style_sidebar, 0);
    lv_style_set_border_width(&s_style_sidebar, 0);
    lv_style_set_pad_all(&s_style_sidebar, 0);

    lv_style_init(&s_style_item);
    lv_style_set_bg_opa(&s_style_item, LV_OPA_TRANSP);
    lv_style_set_radius(&s_style_item, 0);
    lv_style_set_border_width(&s_style_item, 0);
    lv_style_set_pad_all(&s_style_item, 0);

    lv_style_init(&s_style_item_label_inactive);
    lv_style_set_text_color(&s_style_item_label_inactive, lv_color_hex(COLOR_TEXT_INACTIVE));
    lv_style_set_text_align(&s_style_item_label_inactive, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&s_style_item_label_active);
    lv_style_set_text_color(&s_style_item_label_active, lv_color_hex(COLOR_TEXT_ACTIVE));
    lv_style_set_text_align(&s_style_item_label_active, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&s_style_accent_bar);
    lv_style_set_bg_color(&s_style_accent_bar, lv_color_hex(COLOR_PRIMARY));
    lv_style_set_bg_opa(&s_style_accent_bar, LV_OPA_COVER);
    lv_style_set_radius(&s_style_accent_bar, 0);

    lv_style_init(&s_style_content);
    lv_style_set_bg_color(&s_style_content, lv_color_hex(COLOR_BG));
    lv_style_set_bg_opa(&s_style_content, LV_OPA_COVER);
    lv_style_set_radius(&s_style_content, 0);
    lv_style_set_border_width(&s_style_content, 0);
    lv_style_set_pad_all(&s_style_content, 0);

    lv_style_init(&s_style_estop);
    lv_style_set_bg_color(&s_style_estop, lv_color_hex(COLOR_DANGER));
    lv_style_set_bg_opa(&s_style_estop, LV_OPA_COVER);
    lv_style_set_radius(&s_style_estop, 6);
    lv_style_set_border_width(&s_style_estop, 0);

    lv_style_init(&s_style_estop_label);
    lv_style_set_text_color(&s_style_estop_label, lv_color_hex(0xFFFFFF));
    /* Narrower sidebar (76px, shrunk from 92px) leaves the E-Stop button
     * only ~60px wide - the default theme font made "WARNING STOP" overflow
     * it. */
    lv_style_set_text_font(&s_style_estop_label, &lv_font_montserrat_12);

    lv_style_init(&s_style_alarm_banner);
    lv_style_set_bg_color(&s_style_alarm_banner, lv_color_hex(0xB71C1C));
    lv_style_set_bg_opa(&s_style_alarm_banner, LV_OPA_COVER);
    lv_style_set_radius(&s_style_alarm_banner, 0);
    lv_style_set_border_width(&s_style_alarm_banner, 0);
    lv_style_set_pad_all(&s_style_alarm_banner, 8);

    lv_style_init(&s_style_alarm_label);
    lv_style_set_text_color(&s_style_alarm_label, lv_color_hex(0xFFFFFF));

    lv_style_init(&s_style_alarm_btn);
    lv_style_set_bg_color(&s_style_alarm_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&s_style_alarm_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_style_alarm_btn, 6);
    lv_style_set_border_width(&s_style_alarm_btn, 0);

    lv_style_init(&s_style_alarm_btn_label);
    lv_style_set_text_color(&s_style_alarm_btn_label, lv_color_hex(0xB71C1C));

    s_styles_ready = true;
}

static void update_tab_styles(void)
{
    for (int i = 0; i < NAV_SHELL_TAB_COUNT; i++) {
        nav_tab_entry_t *tab = &s_tabs[i];
        if (tab->item == NULL) {
            continue;
        }
        bool active = ((nav_shell_tab_t)i == s_current_tab);
        lv_obj_add_style(tab->item_label, active ? &s_style_item_label_active : &s_style_item_label_inactive,
                          LV_PART_MAIN);
        lv_obj_remove_style(tab->item_label, active ? &s_style_item_label_inactive : &s_style_item_label_active,
                             LV_PART_MAIN);
        if (active) {
            lv_obj_clear_flag(tab->accent_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tab->accent_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void switch_to_tab(nav_shell_tab_t tab)
{
    if (tab == s_current_tab || s_tabs[tab].label == NULL) {
        return;
    }
    if (s_current_tab < NAV_SHELL_TAB_COUNT && s_tabs[s_current_tab].hide_fn != NULL) {
        s_tabs[s_current_tab].hide_fn();
    }
    lv_obj_clean(s_content);
    s_current_tab = tab;
    update_tab_styles();
    if (s_tabs[tab].show_fn != NULL) {
        s_tabs[tab].show_fn(s_content);
    }
}

/* Presets/History/Config are locked out while a roast is actively running
 * (operator preference: navigating away from Roast used to lose the live
 * chart's in-progress readings when coming back - simpler/clearer to just
 * prevent leaving to those 3 tabs at all than to keep patching the
 * chart-replay-on-return mechanism). Manual stays reachable (fan/heater
 * override is still useful mid-roast). */
static bool is_tab_locked_during_roast(nav_shell_tab_t tab)
{
    return tab == NAV_SHELL_TAB_PRESETS || tab == NAV_SHELL_TAB_HISTORY || tab == NAV_SHELL_TAB_CONFIG;
}

static bool roast_is_active(void)
{
    const roast_session_t *session = session_sm_get_state();
    return session->phase != ROAST_PHASE_IDLE && session->phase != ROAST_PHASE_COMPLETED &&
           session->phase != ROAST_PHASE_ABORTED;
}

static void tab_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    nav_shell_tab_t tab = (nav_shell_tab_t)(intptr_t)lv_event_get_user_data(e);
    if (is_tab_locked_during_roast(tab) && roast_is_active()) {
        return; /* Defensive - the item's CLICKABLE flag should already prevent this (see alarm_check_timer_cb). */
    }
    switch_to_tab(tab);
}

static void estop_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    command_dispatcher_emergency_stop(SAFETY_CMD_SOURCE_DISPLAY);
}

/* T024/FR-029: critical alarm banner, mandatory manual acknowledgment. */
static const char *alarm_text(safety_alarm_type_t alarm)
{
    switch (alarm) {
    case SAFETY_ALARM_TEMP_ABSOLUTE_CUTOFF:
        return LV_SYMBOL_WARNING " TEMP CUTOFF: bean temp reached 260C, heater OFF";
    case SAFETY_ALARM_SENSOR_FAILURE:
        return LV_SYMBOL_WARNING " SENSOR FAILURE: invalid temperature reading, heater OFF";
    case SAFETY_ALARM_FAN_FAILURE_INDIRECT:
        return LV_SYMBOL_WARNING " FAN FAILURE: abnormal heating pattern detected, heater OFF";
    case SAFETY_ALARM_DURATION_WATCHDOG:
        return LV_SYMBOL_WARNING " MAX DURATION REACHED: auto-cooling forced";
    case SAFETY_ALARM_EMERGENCY_STOP:
        return LV_SYMBOL_WARNING " EMERGENCY STOP activated";
    default:
        return "";
    }
}

static void alarm_ack_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    command_dispatcher_acknowledge_alarm(SAFETY_CMD_SOURCE_DISPLAY);
}

static void alarm_check_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bool needs_ack = false;
    safety_alarm_type_t alarm = safety_manager_get_active_alarm(&needs_ack);
    if (alarm != SAFETY_ALARM_NONE && needs_ack) {
        lv_label_set_text(s_alarm_label, alarm_text(alarm));
        lv_obj_clear_flag(s_alarm_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_alarm_banner);
    } else {
        lv_obj_add_flag(s_alarm_banner, LV_OBJ_FLAG_HIDDEN);
    }

    bool locked = roast_is_active();
    for (int i = 0; i < NAV_SHELL_TAB_COUNT; i++) {
        nav_tab_entry_t *tab = &s_tabs[i];
        if (tab->item == NULL) {
            continue;
        }
        bool should_lock = locked && is_tab_locked_during_roast((nav_shell_tab_t)i);
        if (should_lock) {
            lv_obj_clear_flag(tab->item, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(tab->item, LV_OPA_50, LV_PART_MAIN);
        } else {
            lv_obj_add_flag(tab->item, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(tab->item, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
}

void nav_shell_register_tab(nav_shell_tab_t tab, const char *label,
                             nav_shell_show_fn_t show_fn, nav_shell_hide_fn_t hide_fn)
{
    if (tab >= NAV_SHELL_TAB_COUNT) {
        return;
    }
    s_tabs[tab].label = label;
    s_tabs[tab].show_fn = show_fn;
    s_tabs[tab].hide_fn = hide_fn;
}

/* T055: known tabs render their sidebar text from the i18n catalog (so the
 * language switch in settings_hub.c takes effect immediately, matching
 * FR-038/quickstart.md Cenario 8) instead of the literal string main.c
 * registered - the icon glyph (LV_SYMBOL_*) stays fixed since it's not
 * translatable text. Falls back to the originally registered `fallback`
 * string for any tab not in this table (defensive - keeps this file usable
 * even if a future tab is added here without an i18n entry yet). */
static const char *build_tab_label_text(nav_shell_tab_t tab, const char *fallback, char *out, size_t out_len)
{
    const char *icon;
    i18n_key_t key;
    switch (tab) {
    case NAV_SHELL_TAB_ROAST:   icon = LV_SYMBOL_PLAY;      key = I18N_KEY_NAV_ROAST;   break;
    case NAV_SHELL_TAB_MANUAL:  icon = LV_SYMBOL_TINT;      key = I18N_KEY_NAV_MANUAL;  break;
    case NAV_SHELL_TAB_PRESETS: icon = LV_SYMBOL_LIST;      key = I18N_KEY_NAV_PRESETS; break;
    case NAV_SHELL_TAB_HISTORY: icon = LV_SYMBOL_DIRECTORY; key = I18N_KEY_NAV_HISTORY; break;
    case NAV_SHELL_TAB_CONFIG:  icon = LV_SYMBOL_SETTINGS;  key = I18N_KEY_NAV_CONFIG;  break;
    default: return fallback;
    }
    snprintf(out, out_len, "%s\n%s", icon, i18n_get(key));
    return out;
}

/** Re-renders every registered tab's sidebar label from the current
 * language - called by settings_hub.c right after i18n_set_language(). */
void nav_shell_refresh_labels(void)
{
    for (int i = 0; i < NAV_SHELL_TAB_COUNT; i++) {
        nav_tab_entry_t *tab = &s_tabs[i];
        if (tab->item_label == NULL) {
            continue;
        }
        char label_buf[32];
        lv_label_set_text(tab->item_label, build_tab_label_text((nav_shell_tab_t)i, tab->label, label_buf, sizeof(label_buf)));
    }
}

esp_err_t nav_shell_init(nav_shell_tab_t initial_tab)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
        return ESP_FAIL;
    }

    ensure_styles();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_add_style(scr, &s_style_screen, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_sidebar = lv_obj_create(scr);
    lv_obj_add_style(s_sidebar, &s_style_sidebar, LV_PART_MAIN);
    lv_obj_set_size(s_sidebar, SIDEBAR_WIDTH, 272);
    lv_obj_align(s_sidebar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_sidebar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NAV_SHELL_TAB_COUNT; i++) {
        nav_tab_entry_t *tab = &s_tabs[i];
        if (tab->label == NULL) {
            continue;
        }
        lv_obj_t *item = lv_obj_create(s_sidebar);
        lv_obj_add_style(item, &s_style_item, LV_PART_MAIN);
        lv_obj_set_size(item, SIDEBAR_WIDTH, TAB_HEIGHT);
        lv_obj_align(item, LV_ALIGN_TOP_LEFT, 0, i * TAB_HEIGHT);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *accent = lv_obj_create(item);
        lv_obj_add_style(accent, &s_style_accent_bar, LV_PART_MAIN);
        lv_obj_set_size(accent, 3, TAB_HEIGHT);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(accent, LV_OBJ_FLAG_HIDDEN);
        tab->accent_bar = accent;

        lv_obj_t *lbl = lv_label_create(item);
        lv_obj_add_style(lbl, &s_style_item_label_inactive, LV_PART_MAIN);
        char label_buf[32];
        lv_label_set_text(lbl, build_tab_label_text(i, tab->label, label_buf, sizeof(label_buf)));
        lv_obj_center(lbl);
        tab->item = item;
        tab->item_label = lbl;
    }

    /* Emergency Stop (FR-027): pinned at the bottom of the sidebar, visible
     * on every tab regardless of which screen is currently shown. */
    lv_obj_t *estop = lv_obj_create(s_sidebar);
    lv_obj_add_style(estop, &s_style_estop, LV_PART_MAIN);
    lv_obj_set_size(estop, SIDEBAR_WIDTH - 16, ESTOP_HEIGHT - 16);
    lv_obj_align(estop, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(estop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(estop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(estop, estop_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *estop_lbl = lv_label_create(estop);
    lv_obj_add_style(estop_lbl, &s_style_estop_label, LV_PART_MAIN);
    lv_label_set_text(estop_lbl, LV_SYMBOL_WARNING " STOP");
    lv_obj_center(estop_lbl);

    s_content = lv_obj_create(scr);
    lv_obj_add_style(s_content, &s_style_content, LV_PART_MAIN);
    lv_obj_set_size(s_content, 480 - SIDEBAR_WIDTH, 272);
    lv_obj_align(s_content, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* Critical alarm banner (T024/FR-029): parented directly to `scr` (not
     * s_content) so it spans the full screen width and stays on top
     * regardless of which tab is active; starts hidden. */
    s_alarm_banner = lv_obj_create(scr);
    lv_obj_remove_style_all(s_alarm_banner);
    lv_obj_add_style(s_alarm_banner, &s_style_alarm_banner, LV_PART_MAIN);
    lv_obj_set_size(s_alarm_banner, 480, 44);
    lv_obj_align(s_alarm_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_alarm_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alarm_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(s_alarm_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_alarm_banner, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_alarm_label = lv_label_create(s_alarm_banner);
    lv_obj_add_style(s_alarm_label, &s_style_alarm_label, LV_PART_MAIN);
    lv_label_set_long_mode(s_alarm_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_alarm_label, 340);
    lv_label_set_text(s_alarm_label, "");

    lv_obj_t *ack_btn = lv_btn_create(s_alarm_banner);
    lv_obj_add_style(ack_btn, &s_style_alarm_btn, LV_PART_MAIN);
    lv_obj_set_size(ack_btn, 110, 32);
    lv_obj_add_event_cb(ack_btn, alarm_ack_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ack_lbl = lv_label_create(ack_btn);
    lv_obj_add_style(ack_lbl, &s_style_alarm_btn_label, LV_PART_MAIN);
    lv_label_set_text(ack_lbl, "ACK");
    lv_obj_center(ack_lbl);

    if (s_alarm_timer != NULL) {
        lv_timer_del(s_alarm_timer);
    }
    s_alarm_timer = lv_timer_create(alarm_check_timer_cb, 300, NULL);

    /* Root cause of the "blank on first boot" bug: right after creating a
     * brand-new object tree, lv_obj_get_width()/get_height() on s_content
     * could still report 0x0 (confirmed via boot log: "root=0x0 chart=0x0")
     * because LVGL hadn't resolved/cached its coordinates yet - the very
     * next line (switch_to_tab) immediately builds the Roast tab's content,
     * which reads lv_obj_get_width(s_content) to size everything. Forcing a
     * layout pass here guarantees s_content already reports its real size
     * before any tab content is built. */
    lv_obj_update_layout(scr);

    s_current_tab = NAV_SHELL_TAB_COUNT;
    switch_to_tab(initial_tab);

    /* Force an immediate synchronous redraw/flush before returning, instead
     * of relying on esp_lvgl_port's own render task to get scheduled. Boot
     * log evidence: WiFi driver init (which runs right after this, at high
     * task priority) was starving the LVGL render task for seconds, so the
     * first real screen paint happened long after "Nav shell initialized"
     * was logged - the screen looked stuck/blank during that gap. */
    lv_refr_now(NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Nav shell initialized");
    return ESP_OK;
}

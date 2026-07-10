/**
 * @file main.c
 * @brief Pop Roaster firmware entry point.
 *
 * Boot sequence (see specs/001-pop-roaster-control/plan.md and research.md):
 *   1. Init NVS + storage layer (profiles, calibration, config, sessions).
 *   2. Init HAL drivers (MAX6675, SSR heater, fan PWM) using board_config.h pins.
 *   3. Init Safety Manager (must exist before any actuator can be commanded).
 *   4. Init display (esp_lcd_nv3041 + esp_lvgl_port) and touch (GT911).
 *   5. Init WiFi provisioning (AP+captive portal on first boot, else STA).
 *   6. Init HTTP/WebSocket server + Artisan adapter.
 *   7. Attempt session recovery (power-loss auto-resume, FR-022).
 *   8. Hand off to the UI/roast_core event loop.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "storage/nvs_store.h"
#include "storage/session_store.h"
#include "storage/profile_store.h"
#include "hal/max6675.h"
#include "hal/ssr_heater.h"
#include "hal/fan_pwm.h"
#include "hal/wifi_provisioning.h"
#include "hal/touch_driver.h"
#include "safety/safety_manager.h"
#include "safety/duration_watchdog.h"
#include "safety/fan_failure_detector.h"
#include "roast_core/session_state_machine.h"
#include "roast_core/command_dispatcher.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/profile_curve_follower.h"
#include "roast_core/session_recovery.h"
#include "web_api/server.h"
#include "ui_display/i18n.h"
#include "ui_display/display_panel.h"
#include "ui_display/widgets/nav_shell.h"
#include "ui_display/screens/roast_dashboard.h"
#include "ui_display/screens/manual_control.h"
#include "ui_display/screens/profile_list.h"
#include "ui_display/screens/session_review.h"
#include "ui_display/screens/placeholder_screen.h"

/* TODO(T047): #include "artisan_adapter/artisan_bridge.h" once the Artisan bridge is implemented. */

static const char *TAG = "pop_roaster_main";

static void config_tab_show(lv_obj_t *parent)
{
    placeholder_screen_show_in(parent, "Config",
        "Language switch, sensor calibration, peripheral tests and Wi-Fi setup will be consolidated here (FR-046).");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pop Roaster booting...");

    /* NVS must be initialized before anything else touches flash storage. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ESP_ERROR_CHECK(nvs_store_init());
    ESP_ERROR_CHECK(session_store_init());
    ESP_ERROR_CHECK(profile_store_init());
    ESP_ERROR_CHECK(i18n_init());

    /* Peripheral drivers - pins come from board_config.h (Kconfig-driven). */
    ESP_ERROR_CHECK(max6675_init());
    ESP_ERROR_CHECK(ssr_heater_init());
    ESP_ERROR_CHECK(fan_pwm_init());

    /* Safety Manager must be up before the state machine can accept commands. */
    ESP_ERROR_CHECK(safety_manager_init());
    ESP_ERROR_CHECK(session_state_machine_init());
    ESP_ERROR_CHECK(command_dispatcher_init());
    ESP_ERROR_CHECK(roast_telemetry_service_init());
    ESP_ERROR_CHECK(profile_curve_follower_init());
    ESP_ERROR_CHECK(duration_watchdog_init());
    ESP_ERROR_CHECK(fan_failure_detector_init());

    /* Display / touch bring-up (fixed board pins from board_config.h).
     * Touch failure is NOT fatal: the display/WiFi/web should still come up
     * even if the GT911 glitches on this boot (ui_display_panel_init()
     * already tolerates a missing touch handle). */
    esp_err_t touch_err = touch_driver_init();
    if (touch_err != ESP_OK) {
        ESP_LOGE(TAG, "touch_driver_init failed (continuing without touch): %s", esp_err_to_name(touch_err));
    }
    ESP_ERROR_CHECK(ui_display_panel_init());

    /* Persistent left navigation sidebar (FR-045): register each tab's
     * content renderer before building the shell. Labels include a small
     * icon line (LV_SYMBOL_*) above the text, per the user's request. */
    nav_shell_register_tab(NAV_SHELL_TAB_ROAST, LV_SYMBOL_PLAY "\nRoast", roast_dashboard_show_in, roast_dashboard_hide);
    nav_shell_register_tab(NAV_SHELL_TAB_PRESETS, LV_SYMBOL_LIST "\nPresets", profile_list_show_in, NULL);
    nav_shell_register_tab(NAV_SHELL_TAB_MANUAL, LV_SYMBOL_TINT "\nManual", manual_control_show_in, manual_control_hide);
    nav_shell_register_tab(NAV_SHELL_TAB_HISTORY, LV_SYMBOL_DIRECTORY "\nHistory", session_review_show_in, NULL);
    nav_shell_register_tab(NAV_SHELL_TAB_CONFIG, LV_SYMBOL_SETTINGS "\nConfig", config_tab_show, NULL);
    ESP_ERROR_CHECK(nav_shell_init(NAV_SHELL_TAB_ROAST));

    ESP_ERROR_CHECK(wifi_provisioning_init());
    ESP_ERROR_CHECK(web_api_server_init());
    /* TODO(T047): ESP_ERROR_CHECK(artisan_bridge_init()); once the Artisan bridge is implemented. */

    /* FR-022: attempt to resume a roast that was active before a power loss. */
    ESP_ERROR_CHECK(session_recovery_try_resume());

    ESP_LOGI(TAG, "Pop Roaster boot complete");
}

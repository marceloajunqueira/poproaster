/**
 * @file artisan_bridge.c
 * @brief See header.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbcontroller.h"

#include "hal/wifi_provisioning.h"
#include "roast_core/roast_telemetry_service.h"
#include "roast_core/command_dispatcher.h"
#include "safety/safety_manager.h"
#include "artisan_adapter/artisan_bridge.h"

static const char *TAG = "artisan_bridge";

#define MB_TCP_PORT_NUMBER 502
#define MB_SLAVE_ADDR      1

#define REG_BT_X10      0
#define REG_FAN_PCT     1
#define REG_HEATER_PCT  2
#define REG_ROR_X10     3
#define REG_SET_FAN     100
#define REG_SET_HEATER  101
#define REG_COUNT       128 /* Comfortable margin past the highest used index (101). */

#define POLL_PERIOD_MS 500

/* Plain array esp-modbus reads/writes directly (no separate serialization
 * step) - the whole point of the "register area" API. Not `static` inside
 * a function since esp-modbus needs a stable address for the lifetime of
 * the slave (registered once via mbc_slave_set_descriptor()). */
static uint16_t s_holding_regs[REG_COUNT];
static void *s_mbc_slave_handle;

/* Tracks what we last SENT to the hardware for registers 100/101, so the
 * polling task only issues a new command_dispatcher call when Artisan
 * actually writes a new value (not every single poll tick). */
static int s_last_applied_fan = -1;
static int s_last_applied_heater = -1;

static void artisan_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));

        /* Telemetry: always published, regardless of control mode (T047). */
        roast_telemetry_snapshot_t snap;
        roast_telemetry_service_get_snapshot(&snap);

        s_holding_regs[REG_BT_X10] = snap.sensor_valid ? (uint16_t)(snap.bean_temp_c * 10.0f) : 0;
        s_holding_regs[REG_FAN_PCT] = (uint16_t)snap.fan_pct;
        s_holding_regs[REG_HEATER_PCT] = (uint16_t)snap.heater_pct;
        s_holding_regs[REG_ROR_X10] = (int16_t)(snap.ror_c_per_min * 10.0f);

        /* Commands: only actually applied while in Manual/Artisan mode -
         * command_dispatcher_set_fan_pct/set_heater_pct already silently
         * ignore SAFETY_CMD_SOURCE_ARTISAN while ROAST_MODE_PROFILE is
         * active (US3 Acceptance Scenario 4), so no extra mode check is
         * needed here - just forward whatever Artisan wrote. */
        int fan_cmd = (int)s_holding_regs[REG_SET_FAN];
        if (fan_cmd != s_last_applied_fan && fan_cmd >= 0 && fan_cmd <= 100) {
            esp_err_t err = command_dispatcher_set_fan_pct((uint8_t)fan_cmd, SAFETY_CMD_SOURCE_ARTISAN);
            if (err == ESP_OK) {
                s_last_applied_fan = fan_cmd;
            } else {
                ESP_LOGW(TAG, "Artisan fan command %d%% rejected: %s", fan_cmd, esp_err_to_name(err));
            }
        }

        int heater_cmd = (int)s_holding_regs[REG_SET_HEATER];
        if (heater_cmd != s_last_applied_heater && heater_cmd >= 0 && heater_cmd <= 100) {
            esp_err_t err = command_dispatcher_set_heater_pct((uint8_t)heater_cmd, SAFETY_CMD_SOURCE_ARTISAN);
            if (err == ESP_OK) {
                s_last_applied_heater = heater_cmd;
            } else {
                ESP_LOGW(TAG, "Artisan heater command %d%% rejected: %s", heater_cmd, esp_err_to_name(err));
            }
        }
    }
}

esp_err_t artisan_bridge_init(void)
{
    memset(s_holding_regs, 0, sizeof(s_holding_regs));

    /* esp-modbus v2.x API: a single mb_communication_info_t union, TCP
     * variant accessed via the `.tcp_opts` member (v1.x had flat
     * ip_port/ip_mode/ip_addr/ip_netif_ptr fields directly on the struct -
     * that layout no longer exists in v2). */
    mb_communication_info_t comm_info = {0};
    comm_info.tcp_opts.mode = MB_TCP;
    comm_info.tcp_opts.port = MB_TCP_PORT_NUMBER;
    comm_info.tcp_opts.uid = MB_SLAVE_ADDR;
    comm_info.tcp_opts.addr_type = MB_IPV4;
    comm_info.tcp_opts.ip_addr_table = NULL; /* Listen on all interfaces. */
    comm_info.tcp_opts.ip_netif_ptr = (void *)wifi_provisioning_get_sta_netif();

    esp_err_t err = mbc_slave_create_tcp(&comm_info, &s_mbc_slave_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_create_tcp failed: %s", esp_err_to_name(err));
        return err;
    }

    mb_register_area_descriptor_t reg_area = {0};
    reg_area.type = MB_PARAM_HOLDING;
    reg_area.start_offset = 0;
    reg_area.address = (void *)s_holding_regs;
    reg_area.size = sizeof(s_holding_regs);
    err = mbc_slave_set_descriptor(s_mbc_slave_handle, reg_area);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_set_descriptor failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mbc_slave_start(s_mbc_slave_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_start failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(artisan_poll_task, "artisan_bridge", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create artisan_bridge poll task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Artisan Modbus TCP bridge listening on port %d (slave id %d)", MB_TCP_PORT_NUMBER, MB_SLAVE_ADDR);
    return ESP_OK;
}

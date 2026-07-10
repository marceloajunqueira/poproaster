/**
 * @file artisan_bridge.h
 * @brief T047: Artisan integration via Modbus TCP (port 502) - Artisan's own
 *        built-in, network-capable device protocol (confirmed from Artisan's
 *        source, artisanlib/modbusport.py: `AsyncModbusTcpClient`, configurable
 *        host/port/device-id/register/function-code per channel). No extra
 *        software is needed on the PC side - just point Artisan's Modbus
 *        device config at this ESP32's IP address, port 502.
 *
 * In Artisan: Config > Device... (NOT the "equipamento"/Machine preset
 * wizard, which only lists specific commercial brands/models and doesn't
 * apply here) - set the Meter/PID type to Modbus, "TCP" mode, host = this
 * device's IP (see the Config tab on the display, or the web UI's Config
 * page), port 502, and map each channel to the holding registers below
 * (function code 3 = Read Holding Registers to read; function code 6 =
 * Write Single Register to write the Set Fan/Set Heater registers via a
 * custom Artisan slider/button "Modbus" action).
 *
 * Register map (all plain 16-bit Holding Registers, device/slave id 1,
 * scaled-integer convention - set Artisan's per-channel "Div" dropdown as
 * noted so Artisan shows the real decimal value; this avoids any
 * float/word-order ambiguity entirely):
 *   - Register 0: Bean Temp x10 (e.g. 2350 = 235.0C) - Artisan channel Div=1/10, unsigned
 *   - Register 1: Fan % (0-100, whole number)        - Div=none, unsigned
 *   - Register 2: Heater % (0-100, whole number)      - Div=none, unsigned
 *   - Register 3: RoR x10 (can be negative during cooling) - Div=1/10, SIGNED
 *   - Register 100: Set Fan % (0-100)    - WRITE via Artisan custom button/slider (function 6)
 *   - Register 101: Set Heater % (0-100) - WRITE via Artisan custom button/slider (function 6)
 *
 * Writes to registers 100/101 are routed through command_dispatcher.h with
 * SAFETY_CMD_SOURCE_ARTISAN - same Profile-mode-Artisan-read-only gate
 * (US3 Acceptance Scenario 4) and Safety Manager validation as every other
 * command source: while the active session is in ROAST_MODE_PROFILE these
 * writes are silently ignored for control (telemetry keeps flowing); they
 * are only actually applied while in ROAST_MODE_MANUAL_ARTISAN.
 *
 * Full step-by-step Artisan-side setup guide (Portuguese): see
 * firmware/docs/artisan-integration.md.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the Modbus TCP slave and its background telemetry/command polling task. Call once at boot, after wifi_provisioning_init(), command_dispatcher_init() and roast_telemetry_service_init(). */
esp_err_t artisan_bridge_init(void);

#ifdef __cplusplus
}
#endif

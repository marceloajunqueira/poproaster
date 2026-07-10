/**
 * @file board_config.h
 * @brief Board abstraction contract for Pop Roaster displays/boards.
 *
 * HOW TO ADD A NEW DISPLAY/BOARD IN THE FUTURE
 * ---------------------------------------------------------------------------
 * 1. Add a new `POPROASTER_BOARD_<NAME>` choice entry in Kconfig.
 * 2. Create `boards/board_<name>.h` defining every macro listed in the
 *    "Required board macros" section below (display pins/resolution/driver,
 *    touch pins/controller).
 * 3. Add an `#elif defined(CONFIG_POPROASTER_BOARD_<NAME>)` branch below
 *    including your new header.
 * 4. Do NOT change any code in roast_core/, safety/, ui_display/ screens, or
 *    web_api/ - they must only ever consume the macros defined here, never
 *    hardcode pins or resolutions, so business logic stays portable across
 *    displays (per FR-017 in specs/001-pop-roaster-control/spec.md).
 *
 * External peripheral pins (MAX6675, SSR heater, fan PWM) are NOT part of the
 * board's fixed display hardware - they are wired by the user via the
 * board's JST1.25 expansion connectors and are fully configurable via
 * `idf.py menuconfig` (see Kconfig in this component), independent of which
 * display board is selected.
 */
#pragma once

#include "sdkconfig.h"

/* -------------------------------------------------------------------------
 * Select the concrete board header based on Kconfig choice.
 * ---------------------------------------------------------------------- */
#if defined(CONFIG_POPROASTER_BOARD_JC4827W543)
#include "boards/board_jc4827w543.h"
#else
#error "No Pop Roaster board selected. Run 'idf.py menuconfig' -> Pop Roaster Board Configuration."
#endif

/* -------------------------------------------------------------------------
 * Required board macros (every boards/board_<name>.h file MUST define all
 * of these; listed here purely as documentation/contract, not defaults).
 *
 * MCU / memory (informational):
 *   BOARD_MCU_NAME              e.g. "ESP32-S3-WROOM-1-N4R8"
 *   BOARD_PSRAM_MB              e.g. 8
 *   BOARD_FLASH_MB              e.g. 4
 *
 * Display panel:
 *   BOARD_DISPLAY_DRIVER_NAME   e.g. "NV3041A"
 *   BOARD_DISPLAY_WIDTH_PX      e.g. 480
 *   BOARD_DISPLAY_HEIGHT_PX     e.g. 272
 *   BOARD_DISPLAY_ROTATION      0..3 (LVGL/esp_lcd rotation convention)
 *   BOARD_DISPLAY_BUS_TYPE      e.g. BOARD_DISPLAY_BUS_QSPI
 *   BOARD_DISPLAY_PIN_CS
 *   BOARD_DISPLAY_PIN_SCK
 *   BOARD_DISPLAY_PIN_D0
 *   BOARD_DISPLAY_PIN_D1
 *   BOARD_DISPLAY_PIN_D2
 *   BOARD_DISPLAY_PIN_D3
 *   BOARD_DISPLAY_PIN_RST       (-1 if not wired)
 *   BOARD_DISPLAY_PIN_BACKLIGHT
 *
 * Touch controller:
 *   BOARD_TOUCH_CONTROLLER_NAME e.g. "GT911"
 *   BOARD_TOUCH_BUS_TYPE        e.g. BOARD_TOUCH_BUS_I2C
 *   BOARD_TOUCH_PIN_SDA
 *   BOARD_TOUCH_PIN_SCL
 *   BOARD_TOUCH_PIN_RST
 *   BOARD_TOUCH_PIN_INT
 *   BOARD_TOUCH_I2C_ADDR        7-bit I2C address
 * ---------------------------------------------------------------------- */

/* Bus type identifiers used by BOARD_DISPLAY_BUS_TYPE / BOARD_TOUCH_BUS_TYPE. */
#define BOARD_DISPLAY_BUS_QSPI   1
#define BOARD_DISPLAY_BUS_SPI    2
#define BOARD_DISPLAY_BUS_RGB    3
#define BOARD_TOUCH_BUS_I2C      1
#define BOARD_TOUCH_BUS_SPI      2

/* -------------------------------------------------------------------------
 * External peripheral pins (user-wired via JST1.25 expansion connectors).
 * Sourced from Kconfig so they can be tuned via `idf.py menuconfig` without
 * editing any source file. Same macros regardless of which display board is
 * selected above.
 * ---------------------------------------------------------------------- */
#define BOARD_PERIPH_MAX6675_SCK_GPIO   CONFIG_POPROASTER_MAX6675_SCK_GPIO
#define BOARD_PERIPH_MAX6675_CS_GPIO    CONFIG_POPROASTER_MAX6675_CS_GPIO
#define BOARD_PERIPH_MAX6675_SO_GPIO    CONFIG_POPROASTER_MAX6675_SO_GPIO

#define BOARD_PERIPH_SSR_HEATER_GPIO    CONFIG_POPROASTER_SSR_HEATER_GPIO

#define BOARD_PERIPH_FAN_PWM_GPIO           CONFIG_POPROASTER_FAN_PWM_GPIO
#define BOARD_PERIPH_FAN_PWM_LEDC_TIMER     CONFIG_POPROASTER_FAN_PWM_LEDC_TIMER
#define BOARD_PERIPH_FAN_PWM_LEDC_CHANNEL   CONFIG_POPROASTER_FAN_PWM_LEDC_CHANNEL
#define BOARD_PERIPH_FAN_PWM_FREQ_HZ        CONFIG_POPROASTER_FAN_PWM_FREQ_HZ

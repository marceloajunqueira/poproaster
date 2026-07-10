/**
 * @file board_jc4827w543.h
 * @brief Fixed hardware definitions for the Guition JC4827W543 display board.
 *
 * Sources (see specs/001-pop-roaster-control/research.md Decision 2 for
 * full references and rationale):
 *   - https://github.com/lsdlsd88/JC4827W543 (factory documentation)
 *   - https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board
 *     (community pinout + confirmed GT911 touch wiring)
 *
 * These pins are FIXED by the board's own PCB (display + touch are soldered
 * on-board) and must NOT be exposed via Kconfig - unlike the external
 * peripheral pins (MAX6675/SSR/fan), which are user-wired via the JST1.25
 * expansion connectors and configured in ../include/board_config.h.
 */
#pragma once

#define BOARD_MCU_NAME              "ESP32-S3-WROOM-1-N4R8"
#define BOARD_PSRAM_MB              8   /* Octal PSRAM (OSPI) */
#define BOARD_FLASH_MB              4   /* QSPI flash */

/* ---------------------------------------------------------------------
 * Display panel: NV3041A, 480x272, 16-bit RGB565, 4-wire QSPI interface.
 * ------------------------------------------------------------------- */
#define BOARD_DISPLAY_DRIVER_NAME   "NV3041A"
#define BOARD_DISPLAY_WIDTH_PX      480
#define BOARD_DISPLAY_HEIGHT_PX     272
#define BOARD_DISPLAY_ROTATION      0
#define BOARD_DISPLAY_BUS_TYPE      BOARD_DISPLAY_BUS_QSPI

#define BOARD_DISPLAY_PIN_CS        45
#define BOARD_DISPLAY_PIN_SCK       47
#define BOARD_DISPLAY_PIN_D0        21
#define BOARD_DISPLAY_PIN_D1        48
#define BOARD_DISPLAY_PIN_D2        40
#define BOARD_DISPLAY_PIN_D3        39
#define BOARD_DISPLAY_PIN_RST       (-1) /* not wired on this board */
#define BOARD_DISPLAY_PIN_BACKLIGHT 1

/* ---------------------------------------------------------------------
 * Touch: GT911 capacitive touch controller over I2C.
 * ------------------------------------------------------------------- */
#define BOARD_TOUCH_CONTROLLER_NAME "GT911"
#define BOARD_TOUCH_BUS_TYPE        BOARD_TOUCH_BUS_I2C

#define BOARD_TOUCH_PIN_SCL         4
#define BOARD_TOUCH_PIN_SDA         8
#define BOARD_TOUCH_PIN_RST         38
#define BOARD_TOUCH_PIN_INT         3
#define BOARD_TOUCH_I2C_ADDR        0x5D /* GT911_SLAVE_ADDRESS1 */

/* ---------------------------------------------------------------------
 * JST1.25 expansion connectors (confirmed physical pinout, informational
 * only). Peripheral GPIO assignments below are NOT hardcoded here - they
 * are configurable via Kconfig (see ../Kconfig) so the user can tune them
 * to whichever expansion pins they choose to wire, without touching code.
 *
 *   P1: GND, RXD, TXD, +5V           (UART/power - not general GPIO)
 *   P2: IO46, IO9, IO14, IO5
 *   P3: IO6, IO7, IO15, IO16
 *   P4: GND, 3.3V, IO17, IO18
 *
 * Hardware note: IO46 is INPUT-ONLY on ESP32-S3 - never assign it to an
 * output signal (e.g. SSR or fan PWM). It is well suited to MAX6675 SO/MISO
 * (a read-only signal from the MCU's perspective), which is the default in
 * Kconfig.
 * ------------------------------------------------------------------- */

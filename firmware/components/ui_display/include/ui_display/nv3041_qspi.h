/**
 * @file nv3041_qspi.h
 * @brief Minimal, from-scratch NV3041A QSPI driver using the exact wire
 *        protocol confirmed to work on the JC4827W543 board (reverse
 *        engineered from the community's proven-working Arduino_GFX
 *        Arduino_ESP32QSPI + Arduino_NV3041A implementation):
 *
 *  - Every SPI transaction uses the SPI peripheral's dedicated 8-bit
 *    "command" phase + 24-bit "address" phase (NOT just raw tx_buffer
 *    bytes, which is what the generic esp_lcd_panel_io_spi driver does -
 *    and why it never worked for this board's no-D/C QSPI wiring).
 *  - LCD commands (SWRESET, SLPOUT, MADCTL, CASET, RASET, ...) are sent as:
 *    hardware cmd=0x02 (fixed "this is a command" opcode), hardware
 *    addr=(real_command_byte << 8), then 0+ parameter bytes as plain
 *    (single-line) tx data.
 *  - Pixel/GRAM writes (RAMWR) are sent as: hardware cmd=0x32 (fixed "this
 *    is pixel data" opcode), hardware addr=0x003C00 (fixed GRAM write
 *    address), then the color payload sent over all 4 data lines
 *    (SPI_TRANS_MODE_QIO) for throughput.
 *
 * This intentionally bypasses the `eric-c-e/esp_lcd_nv3041` component and
 * the generic `esp_lcd_panel_io_spi` layer, which only support the classic
 * "D/C pin" or plain single-line command framing - neither of which matches
 * this specific 6-pin (CS/SCK/D0-D3) QSPI-only wiring.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t nv3041_qspi_init(void);

/** Writes a rectangular region of RGB565 pixel data (big-endian byte order) to the panel's GRAM. */
esp_err_t nv3041_qspi_draw_bitmap(int x1, int y1, int x2, int y2, const void *color_data);

/**
 * @file nv3041_qspi.c
 * @brief Minimal, from-scratch NV3041A QSPI driver (see header for the wire
 *        protocol rationale, reverse-engineered from Arduino_GFX's
 *        confirmed-working Arduino_ESP32QSPI + Arduino_NV3041A source).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "board_config.h"
#include "ui_display/nv3041_qspi.h"

static const char *TAG = "nv3041_qspi";

#define NV3041_SPI_HOST      SPI3_HOST /* SPI2_HOST is already used by MAX6675 (hal/max6675.c). */
#define NV3041_PCLK_HZ       (8 * 1000 * 1000) /* Lowered from 20MHz to rule out QSPI signal integrity as the cause of pixel noise. */
#define NV3041_CMD_OPCODE    0x02 /* Fixed "command" framing opcode (SPI-NOR-flash-style Page Program). */
#define NV3041_PIXEL_OPCODE  0x32 /* Fixed "quad pixel write" framing opcode (SPI-NOR-flash-style Quad Page Program). */
#define NV3041_PIXEL_ADDR    0x003C00 /* Fixed GRAM write address used by the pixel-write opcode. */
#define NV3041_MAX_CHUNK_PX  (2048) /* Pixels per SPI transaction chunk during a bulk pixel write. */

static spi_device_handle_t s_spi;

static esp_err_t nv3041_write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.cmd = NV3041_CMD_OPCODE;
    t.addr = ((uint32_t)cmd) << 8;
    t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    if (data != NULL && len > 0) {
        t.tx_buffer = data;
        t.length = len * 8;
    }
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t nv3041_write_pixels_chunk(const uint8_t *data, size_t len_bytes)
{
    spi_transaction_t t = {0};
    t.cmd = NV3041_PIXEL_OPCODE;
    t.addr = NV3041_PIXEL_ADDR;
    t.flags = SPI_TRANS_MODE_QIO;
    t.tx_buffer = data;
    t.length = len_bytes * 8;
    return spi_device_polling_transmit(s_spi, &t);
}

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
} nv3041_init_cmd_t;

#define D1(x) (const uint8_t[]){x}

/*
 * Init sequence transcribed from the community's Arduino_NV3041A driver's
 * `nv3041a_init_operations` table (bundled Arduino_GFX-1.4.4 in
 * profi-max/JC4827W543_4.3inch_ESP32S3_board), NOT the eric-c-e/esp_lcd_nv3041
 * vendor defaults we started with. The two differ substantially in the
 * analog bias/contrast trim registers (0x7A/0x6F/0x78/0xC1/0xC2/0xC6/0x51-0x54)
 * and use a completely different (individual single-byte register) gamma
 * table encoding (0x80-0x92 / 0xA0-0xB2) instead of two 19-byte block
 * writes - using the vendor defaults produced washed-out/low-contrast
 * colors on this exact board; this sequence is confirmed to look correct
 * (factory-demo quality) on this hardware.
 */
static const nv3041_init_cmd_t INIT_CMDS[] = {
    {0x11, NULL,        0, 100}, /* SLPOUT */
    {0xff, D1(0xa5),    1, 0},   /* enable registers (undocumented) */
    {0x36, D1(0xc0),    1, 0},   /* MADCTL */
    {0x3a, D1(0x01),    1, 0},   /* COLMOD: 16bpp RGB565 */
    {0x41, D1(0x03),    1, 0},   /* Bus Width: 16-bit */
    {0x44, D1(0x15),    1, 0},   /* VBP */
    {0x45, D1(0x15),    1, 0},   /* VFP */
    {0x7d, D1(0x03),    1, 0},   /* vdds_trim */
    {0xc1, D1(0xab),    1, 0},   /* avdd/avcl clamp */
    {0xc2, D1(0x17),    1, 0},   /* vgl clamp */
    {0xc3, D1(0x10),    1, 0},
    {0xc6, D1(0x3a),    1, 0},
    {0xc7, D1(0x25),    1, 0},
    {0xc8, D1(0x11),    1, 0},   /* VGL_CLK_sel */
    {0x7a, D1(0x49),    1, 0},   /* user_vgsp */
    {0x6f, D1(0x2f),    1, 0},   /* user_gvdd */
    {0x78, D1(0x4b),    1, 0},   /* user_gvcl */
    {0xc9, D1(0x00),    1, 0},
    {0x67, D1(0x33),    1, 0},
    {0x51, D1(0x4b),    1, 0},   /* gate_st_o */
    {0x52, D1(0x7c),    1, 0},   /* gate_ed_o */
    {0x53, D1(0x1c),    1, 0},   /* gate_st_e */
    {0x54, D1(0x77),    1, 0},   /* gate_ed_e */
    {0x46, D1(0x0a),    1, 0},   /* fsm_hbp_o */
    {0x47, D1(0x2a),    1, 0},   /* fsm_hfp_o */
    {0x48, D1(0x0a),    1, 0},   /* fsm_hbp_e */
    {0x49, D1(0x1a),    1, 0},   /* fsm_hfp_e */
    {0x56, D1(0x43),    1, 0},
    {0x57, D1(0x42),    1, 0},
    {0x58, D1(0x3c),    1, 0},
    {0x59, D1(0x64),    1, 0},
    {0x5a, D1(0x41),    1, 0},
    {0x5b, D1(0x3c),    1, 0},
    {0x5c, D1(0x02),    1, 0},
    {0x5d, D1(0x3c),    1, 0},
    {0x5e, D1(0x1f),    1, 0},
    {0x60, D1(0x80),    1, 0},
    {0x61, D1(0x3f),    1, 0},
    {0x62, D1(0x21),    1, 0},
    {0x63, D1(0x07),    1, 0},
    {0x64, D1(0xe0),    1, 0},
    {0x65, D1(0x01),    1, 0},   /* chopper */
    {0xca, D1(0x20),    1, 0},
    {0xcb, D1(0x52),    1, 0},
    {0xcc, D1(0x10),    1, 0},
    {0xcd, D1(0x42),    1, 0},
    {0xd0, D1(0x20),    1, 0},
    {0xd1, D1(0x52),    1, 0},
    {0xd2, D1(0x10),    1, 0},
    {0xd3, D1(0x42),    1, 0},
    {0xd4, D1(0x0a),    1, 0},
    {0xd5, D1(0x32),    1, 0},
    /* Gamma - individual single-byte P/N register pairs (NOT a block write). */
    {0x80, D1(0x04),    1, 0}, {0xa0, D1(0x00),    1, 0},
    {0x81, D1(0x07),    1, 0}, {0xa1, D1(0x05),    1, 0},
    {0x82, D1(0x06),    1, 0}, {0xa2, D1(0x04),    1, 0},
    {0x86, D1(0x2c),    1, 0}, {0xa6, D1(0x2a),    1, 0},
    {0x87, D1(0x46),    1, 0}, {0xa7, D1(0x44),    1, 0},
    {0x83, D1(0x39),    1, 0}, {0xa3, D1(0x39),    1, 0},
    {0x84, D1(0x3a),    1, 0}, {0xa4, D1(0x3a),    1, 0},
    {0x85, D1(0x3f),    1, 0}, {0xa5, D1(0x3f),    1, 0},
    {0x88, D1(0x08),    1, 0}, {0xa8, D1(0x08),    1, 0},
    {0x89, D1(0x0f),    1, 0}, {0xa9, D1(0x0f),    1, 0},
    {0x8a, D1(0x17),    1, 0}, {0xaa, D1(0x17),    1, 0},
    {0x8b, D1(0x10),    1, 0}, {0xab, D1(0x10),    1, 0},
    {0x8c, D1(0x16),    1, 0}, {0xac, D1(0x16),    1, 0},
    {0x8d, D1(0x14),    1, 0}, {0xad, D1(0x14),    1, 0},
    {0x8e, D1(0x11),    1, 0}, {0xae, D1(0x11),    1, 0},
    {0x8f, D1(0x14),    1, 0}, {0xaf, D1(0x14),    1, 0},
    {0x90, D1(0x06),    1, 0}, {0xb0, D1(0x06),    1, 0},
    {0x91, D1(0x0f),    1, 0}, {0xb1, D1(0x0f),    1, 0},
    {0x92, D1(0x16),    1, 0}, {0xb2, D1(0x16),    1, 0},
    {0xff, D1(0x00),    1, 0},
    {0x11, D1(0x00),    1, 120}, /* SLPOUT again per confirmed sequence */
    {0x29, D1(0x00),    1, 100}, /* DISPON */
};

esp_err_t nv3041_qspi_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_DISPLAY_PIN_SCK,
        .data0_io_num = BOARD_DISPLAY_PIN_D0,
        .data1_io_num = BOARD_DISPLAY_PIN_D1,
        .data2_io_num = BOARD_DISPLAY_PIN_D2,
        .data3_io_num = BOARD_DISPLAY_PIN_D3,
        .max_transfer_sz = NV3041_MAX_CHUNK_PX * 2,
    };
    esp_err_t err = spi_bus_initialize(NV3041_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = NV3041_PCLK_HZ,
        .spics_io_num = BOARD_DISPLAY_PIN_CS,
        .queue_size = 4,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(NV3041_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]); i++) {
        const nv3041_init_cmd_t *c = &INIT_CMDS[i];
        err = nv3041_write_cmd_data(c->cmd, c->data, c->len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init cmd 0x%02X failed: %s", c->cmd, esp_err_to_name(err));
            return err;
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    /* This panel is IPS (confirmed by the community's Arduino_NV3041A driver
     * treating this exact board as `ips=true` and sending INVON, not INVOFF,
     * after init) - IPS panels are electrically "color inverted" by nature
     * and need INVON (0x21) to actually display normal (non-inverted, non
     * washed-out) colors. Without it, dark colors render light/washed and
     * saturated colors look muted/shifted - matching what was observed on
     * real hardware. */
    err = nv3041_write_cmd_data(0x21, NULL, 0); /* INVON */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INVON failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NV3041A QSPI init OK (CS=%d SCK=%d D0=%d D1=%d D2=%d D3=%d, %ldMHz)",
             BOARD_DISPLAY_PIN_CS, BOARD_DISPLAY_PIN_SCK, BOARD_DISPLAY_PIN_D0,
             BOARD_DISPLAY_PIN_D1, BOARD_DISPLAY_PIN_D2, BOARD_DISPLAY_PIN_D3,
             (long)(NV3041_PCLK_HZ / 1000000));
    return ESP_OK;
}

esp_err_t nv3041_qspi_draw_bitmap(int x1, int y1, int x2, int y2, const void *color_data)
{
    uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)((x2 - 1) >> 8), (uint8_t)(x2 - 1) };
    uint8_t raset[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)((y2 - 1) >> 8), (uint8_t)(y2 - 1) };

    esp_err_t err = nv3041_write_cmd_data(0x2a, caset, sizeof(caset)); /* CASET */
    if (err != ESP_OK) {
        return err;
    }
    err = nv3041_write_cmd_data(0x2b, raset, sizeof(raset)); /* RASET */
    if (err != ESP_OK) {
        return err;
    }
    /* RAMWR must be sent as its own (data-less) command BEFORE the pixel
     * payload - confirmed against Arduino_NV3041A.cpp's setAddrWindow(),
     * which calls writeCommand(NV3041A_RAMWR) separately before any pixel
     * write call. Skipping this leaves the chip's internal GRAM write
     * pointer unarmed, which is very likely why pixel writes without it
     * produced random-looking noise instead of a stable image. */
    err = nv3041_write_cmd_data(0x2c, NULL, 0); /* RAMWR */
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *bytes = (const uint8_t *)color_data;
    size_t total_px = (size_t)(x2 - x1) * (size_t)(y2 - y1);
    size_t total_bytes = total_px * 2;
    size_t offset = 0;
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        size_t max_chunk_bytes = NV3041_MAX_CHUNK_PX * 2;
        if (chunk > max_chunk_bytes) {
            chunk = max_chunk_bytes;
        }
        err = nv3041_write_pixels_chunk(bytes + offset, chunk);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

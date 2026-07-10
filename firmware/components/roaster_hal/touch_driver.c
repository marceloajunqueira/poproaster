/**
 * @file touch_driver.c
 * @brief GT911 touch controller bring-up via esp_lcd_touch_gt911.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

#include "board_config.h"
#include "hal/touch_driver.h"

static const char *TAG = "touch_driver";

#define TOUCH_I2C_PORT       I2C_NUM_0
#define TOUCH_INIT_MAX_RETRIES 3
#define TOUCH_INIT_RETRY_DELAY_MS 100

static i2c_master_bus_handle_t s_i2c_bus_handle;
static esp_lcd_touch_handle_t s_touch_handle;
static esp_lcd_panel_io_handle_t s_touch_io_handle;

esp_err_t touch_driver_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.dev_addr = BOARD_TOUCH_I2C_ADDR;
    err = esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &tp_io_config, &s_touch_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_DISPLAY_WIDTH_PX,
        .y_max = BOARD_DISPLAY_HEIGHT_PX,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* GT911 occasionally misses its first I2C transaction right after power-up
     * (observed intermittently on hardware) - retry a few times with a short
     * delay before giving up, instead of failing the whole boot on one glitch. */
    for (int attempt = 1; attempt <= TOUCH_INIT_MAX_RETRIES; attempt++) {
        err = esp_lcd_touch_new_i2c_gt911(s_touch_io_handle, &tp_cfg, &s_touch_handle);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_lcd_touch_new_i2c_gt911 attempt %d/%d failed: %s",
                 attempt, TOUCH_INIT_MAX_RETRIES, esp_err_to_name(err));
        if (attempt < TOUCH_INIT_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_INIT_RETRY_DELAY_MS));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 failed after %d attempts: %s",
                 TOUCH_INIT_MAX_RETRIES, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "GT911 touch init OK (SDA=%d SCL=%d RST=%d INT=%d addr=0x%02X)",
             BOARD_TOUCH_PIN_SDA, BOARD_TOUCH_PIN_SCL, BOARD_TOUCH_PIN_RST,
             BOARD_TOUCH_PIN_INT, BOARD_TOUCH_I2C_ADDR);
    return ESP_OK;
}

esp_lcd_touch_handle_t touch_driver_get_handle(void)
{
    return s_touch_handle;
}

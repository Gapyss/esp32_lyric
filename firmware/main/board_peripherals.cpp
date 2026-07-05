#include "board_peripherals.h"

#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_peripherals";
static const i2c_port_t I2C_PORT = I2C_NUM_0;
static const gpio_num_t I2C_SDA = GPIO_NUM_13;
static const gpio_num_t I2C_SCL = GPIO_NUM_14;
static const uint32_t I2C_FREQ_HZ = 400000;
static const uint8_t PCF85063_ADDR = 0x51;
static const uint8_t SHTC3_ADDR = 0x70;

static uint8_t bcd_to_int(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static void read_system_time(BoardIdleMetrics *metrics)
{
    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        return;
    }

    struct tm local = {};
    if (localtime_r(&now, &local) == NULL) {
        return;
    }
    metrics->hour = local.tm_hour;
    metrics->minute = local.tm_min;
    metrics->time_valid = metrics->hour < 24 && metrics->minute < 60;
}

static uint8_t crc8_shtc3(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t board_peripherals_start(void)
{
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = I2C_SDA;
    cfg.scl_io_num = I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = I2C_FREQ_HZ;
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C idle peripherals ready on SDA=%d SCL=%d", I2C_SDA, I2C_SCL);
    }
    return err;
}

static void read_rtc(BoardIdleMetrics *metrics)
{
    uint8_t reg = 0x04;
    uint8_t data[3] = {};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT,
                                                 PCF85063_ADDR,
                                                 &reg,
                                                 1,
                                                 data,
                                                 sizeof(data),
                                                 pdMS_TO_TICKS(50));
    if (err != ESP_OK || (data[0] & 0x80) != 0) {
        metrics->time_valid = false;
        return;
    }
    metrics->minute = bcd_to_int(data[1] & 0x7F);
    metrics->hour = bcd_to_int(data[2] & 0x3F);
    metrics->time_valid = metrics->hour < 24 && metrics->minute < 60;
}

static void shtc3_command(uint16_t command)
{
    uint8_t data[2] = {(uint8_t)(command >> 8), (uint8_t)(command & 0xFF)};
    i2c_master_write_to_device(I2C_PORT, SHTC3_ADDR, data, sizeof(data), pdMS_TO_TICKS(50));
}

static void read_shtc3(BoardIdleMetrics *metrics)
{
    shtc3_command(0x3517); // wake
    vTaskDelay(pdMS_TO_TICKS(1));
    shtc3_command(0x7866); // normal power, clock stretching disabled, T first
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6] = {};
    esp_err_t err = i2c_master_read_from_device(I2C_PORT, SHTC3_ADDR, data, sizeof(data), pdMS_TO_TICKS(50));
    shtc3_command(0xB098); // sleep
    if (err != ESP_OK || crc8_shtc3(data, 2) != data[2] || crc8_shtc3(data + 3, 2) != data[5]) {
        metrics->env_valid = false;
        return;
    }

    const uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t raw_rh = ((uint16_t)data[3] << 8) | data[4];
    metrics->temperature_c_x10 = -450 + (int)((1750LL * raw_t) / 65535LL);
    metrics->humidity_x10 = (int)((1000LL * raw_rh) / 65535LL);
    metrics->env_valid = true;
}

void board_peripherals_read(BoardIdleMetrics *metrics)
{
    if (metrics == NULL) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
    read_rtc(metrics);
    if (!metrics->time_valid) {
        read_system_time(metrics);
    }
    read_shtc3(metrics);
}

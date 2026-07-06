#include "audio_chime.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "board_peripherals.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_chime";

static const uint8_t ES8311_ADDR = 0x18;
static const int SAMPLE_RATE = 16000;
static const int CHANNELS = 2;
static const int CHIME_SAMPLES = SAMPLE_RATE / 2;
static const float PI = 3.14159265358979323846f;

static const gpio_num_t I2S_MCLK = GPIO_NUM_16;
static const gpio_num_t I2S_BCLK = GPIO_NUM_9;
static const gpio_num_t I2S_WS = GPIO_NUM_45;
static const gpio_num_t I2S_DOUT = GPIO_NUM_10;
static const gpio_num_t I2S_DIN = GPIO_NUM_8;
static const gpio_num_t PA_ENABLE = GPIO_NUM_46;

static i2s_chan_handle_t g_tx;
static i2c_master_dev_handle_t g_es8311_dev;
static TaskHandle_t g_task;
static bool g_available;
static volatile bool g_playing;
static int16_t g_pcm[CHIME_SAMPLES * CHANNELS];

static esp_err_t es8311_write(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(g_es8311_dev, data, sizeof(data), 50);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(g_es8311_dev, &reg, 1, value, 1, 50);
}

static esp_err_t es8311_init_codec(void)
{
    uint8_t chip_id = 0;
    esp_err_t err = es8311_read(0xFD, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 probe failed at 0x18: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ES8311 ACK, chip id register 0xFD=0x%02x", chip_id);

    err = es8311_write(0x44, 0x08);
    err |= es8311_write(0x44, 0x08);
    err |= es8311_write(0x01, 0x30);
    err |= es8311_write(0x02, 0x00);
    err |= es8311_write(0x03, 0x10);
    err |= es8311_write(0x16, 0x24);
    err |= es8311_write(0x04, 0x10);
    err |= es8311_write(0x05, 0x00);
    err |= es8311_write(0x0B, 0x00);
    err |= es8311_write(0x0C, 0x00);
    err |= es8311_write(0x10, 0x1F);
    err |= es8311_write(0x11, 0x7F);
    err |= es8311_write(0x00, 0x80);
    err |= es8311_write(0x00, 0x80);
    err |= es8311_write(0x01, 0x3F);
    err |= es8311_write(0x06, 0x03);
    err |= es8311_write(0x07, 0x00);
    err |= es8311_write(0x08, 0xFF);
    err |= es8311_write(0x09, 0x0C);
    err |= es8311_write(0x0A, 0x0C);
    err |= es8311_write(0x13, 0x10);
    err |= es8311_write(0x1B, 0x0A);
    err |= es8311_write(0x1C, 0x6A);
    err |= es8311_write(0x44, 0x58);
    err |= es8311_write(0x17, 0xBF);
    err |= es8311_write(0x0E, 0x02);
    err |= es8311_write(0x12, 0x00);
    err |= es8311_write(0x14, 0x1A);
    err |= es8311_write(0x0D, 0x01);
    err |= es8311_write(0x15, 0x40);
    err |= es8311_write(0x37, 0x08);
    err |= es8311_write(0x45, 0x00);
    err |= es8311_write(0x32, 0xC0);
    err |= es8311_write(0x31, 0x00);
    return err;
}

static void generate_chime(void)
{
    memset(g_pcm, 0, sizeof(g_pcm));
    const float notes[] = {1046.5f, 1318.5f, 1568.0f};
    const int note_len = SAMPLE_RATE * 150 / 1000;
    const int gap_len = SAMPLE_RATE * 25 / 1000;
    int pos = 0;
    for (size_t note = 0; note < sizeof(notes) / sizeof(notes[0]); note++) {
        for (int i = 0; i < note_len && pos < CHIME_SAMPLES; i++, pos++) {
            const float t = (float)i / (float)SAMPLE_RATE;
            const float attack = i < 120 ? (float)i / 120.0f : 1.0f;
            const float decay = expf(-4.0f * (float)i / (float)note_len);
            const float sample = sinf(2.0f * PI * notes[note] * t) * attack * decay;
            const int16_t pcm = (int16_t)(sample * 9500.0f);
            g_pcm[pos * 2] = pcm;
            g_pcm[pos * 2 + 1] = pcm;
        }
        pos += gap_len;
    }
}

static void chime_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!g_playing || !g_available) {
            gpio_set_level(PA_ENABLE, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        gpio_set_level(PA_ENABLE, 1);
        const int pa_level = gpio_get_level(PA_ENABLE);
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(g_tx, g_pcm, sizeof(g_pcm), &written, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG,
                 "chime write err=%s bytes=%u/%u pa=%d",
                 esp_err_to_name(err),
                 (unsigned)written,
                 (unsigned)sizeof(g_pcm),
                 pa_level);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

esp_err_t audio_chime_init(void)
{
    generate_chime();

    i2c_master_bus_handle_t bus = board_peripherals_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t es8311_cfg = {};
    es8311_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    es8311_cfg.device_address = ES8311_ADDR;
    es8311_cfg.scl_speed_hz = 400000;
    esp_err_t err = i2c_master_bus_add_device(bus, &es8311_cfg, &g_es8311_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to add ES8311 I2C device: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t pa_cfg = {};
    pa_cfg.mode = GPIO_MODE_OUTPUT;
    pa_cfg.pin_bit_mask = 1ULL << PA_ENABLE;
    pa_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pa_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&pa_cfg));
    gpio_set_level(PA_ENABLE, 0);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &g_tx, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_OK) {
        i2s_std_config_t std_cfg = {};
        std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
        std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        std_cfg.gpio_cfg.mclk = I2S_MCLK;
        std_cfg.gpio_cfg.bclk = I2S_BCLK;
        std_cfg.gpio_cfg.ws = I2S_WS;
        std_cfg.gpio_cfg.dout = I2S_DOUT;
        std_cfg.gpio_cfg.din = I2S_DIN;
        std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
        std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
        std_cfg.gpio_cfg.invert_flags.ws_inv = false;
        err = i2s_channel_init_std_mode(g_tx, &std_cfg);
        if (err == ESP_OK) {
            err = i2s_channel_enable(g_tx);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = es8311_init_codec();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed; hydration screen will still work");
        return err;
    }

    g_available = true;
    BaseType_t task_ok = xTaskCreate(chime_task, "chime", 4096, NULL, 4, &g_task);
    if (task_ok != pdPASS) {
        g_available = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "audio ready: mclk=%d bclk=%d ws=%d dout=%d din=%d pa=%d",
             I2S_MCLK,
             I2S_BCLK,
             I2S_WS,
             I2S_DOUT,
             I2S_DIN,
             PA_ENABLE);
    return ESP_OK;
}

void audio_chime_start(void)
{
    g_playing = true;
}

void audio_chime_stop(void)
{
    g_playing = false;
    gpio_set_level(PA_ENABLE, 0);
}

bool audio_chime_available(void)
{
    return g_available;
}

bool audio_chime_is_playing(void)
{
    return g_playing;
}

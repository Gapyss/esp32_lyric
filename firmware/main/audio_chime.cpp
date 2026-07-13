#include "audio_chime.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "board_peripherals.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_chime";

static const int SAMPLE_RATE = 16000;
static const int CHANNELS = 2;
static const int CHIME_SAMPLES = SAMPLE_RATE / 2;
static const int OUT_VOLUME = 80;  // 0-100, handled by esp_codec_dev
static const float PI = 3.14159265358979323846f;

// I2S pins for the S3_RLCD_4_2 codec board (see spec.md "Audio").
static const gpio_num_t I2S_MCLK = GPIO_NUM_16;
static const gpio_num_t I2S_BCLK = GPIO_NUM_9;
static const gpio_num_t I2S_WS = GPIO_NUM_45;
static const gpio_num_t I2S_DOUT = GPIO_NUM_10;
static const gpio_num_t I2S_DIN = GPIO_NUM_8;
// Speaker power-amplifier enable. Waveshare docs: drive HIGH to enable, so the
// esp_codec_dev es8311 driver owns this pin with pa_reverted = false.
static const int PA_ENABLE_GPIO = 46;
static const int MCLK_MULTIPLE = 256;

static i2s_chan_handle_t g_tx;
static esp_codec_dev_handle_t g_codec;
static esp_codec_dev_sample_info_t g_sample_cfg;
static TaskHandle_t g_task;
static bool g_available;
static volatile bool g_playing;
static int16_t g_pcm[CHIME_SAMPLES * CHANNELS];

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

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &g_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)MCLK_MULTIPLE;
    std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_MCLK;
    std_cfg.gpio_cfg.bclk = I2S_BCLK;
    std_cfg.gpio_cfg.ws = I2S_WS;
    std_cfg.gpio_cfg.dout = I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_DIN;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(g_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(g_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    i2c_master_bus_handle_t bus = board_peripherals_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGW(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {};
    i2s_data_cfg.port = I2S_NUM_0;
    i2s_data_cfg.tx_handle = g_tx;
    i2s_data_cfg.rx_handle = NULL;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (data_if == NULL) {
        ESP_LOGW(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGW(TAG, "audio_codec_new_gpio failed");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.master_mode = false;  // ESP32 I2S is master; codec is slave
    es8311_cfg.use_mclk = true;
    es8311_cfg.pa_pin = PA_ENABLE_GPIO;
    es8311_cfg.pa_reverted = false;  // active-high enable (Waveshare docs)
    es8311_cfg.hw_gain.pa_voltage = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    es8311_cfg.mclk_div = MCLK_MULTIPLE;
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (es8311_if == NULL) {
        ESP_LOGW(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es8311_if;
    dev_cfg.data_if = data_if;
    g_codec = esp_codec_dev_new(&dev_cfg);
    if (g_codec == NULL) {
        ESP_LOGW(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    g_sample_cfg.bits_per_sample = 16;
    g_sample_cfg.channel = CHANNELS;
    g_sample_cfg.channel_mask = 0x03;
    g_sample_cfg.sample_rate = SAMPLE_RATE;

    int ret = esp_codec_dev_open(g_codec, &g_sample_cfg);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_open failed: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_set_out_vol(g_codec, OUT_VOLUME);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_out_vol failed: %d", ret);
    }

    // DIAGNOSTIC: force the PA enable high and read it back truthfully.
    // INPUT_OUTPUT keeps the input buffer on so gpio_get_level reports the real
    // pad level (a plain OUTPUT pin always reads 0). Driving high is safe: the
    // codec driver also wants it high (active-high enable).
    gpio_set_direction((gpio_num_t)PA_ENABLE_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level((gpio_num_t)PA_ENABLE_GPIO, 1);
    ESP_LOGI(TAG, "PA readback (gpio %d) = %d", PA_ENABLE_GPIO,
             gpio_get_level((gpio_num_t)PA_ENABLE_GPIO));
    return ESP_OK;
}

static void chime_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!g_playing || !g_available) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Codec stays open; when idle the auto-cleared DMA feeds silence, so the
        // amp is quiet without toggling the PA pin on every reminder.
        const int ret = esp_codec_dev_write(g_codec, g_pcm, sizeof(g_pcm));
        ESP_LOGI(TAG, "chime write ret=%d bytes=%u", ret, (unsigned)sizeof(g_pcm));
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

esp_err_t audio_chime_init(void)
{
    generate_chime();

    esp_err_t err = i2s_init();
    if (err != ESP_OK) {
        return err;
    }

    err = codec_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed; reminders will still work silently");
        return err;
    }

    g_available = true;
    BaseType_t task_ok = xTaskCreate(chime_task, "chime", 4096, NULL, 4, &g_task);
    if (task_ok != pdPASS) {
        g_available = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "audio ready: mclk=%d bclk=%d ws=%d dout=%d din=%d pa=%d vol=%d",
             I2S_MCLK,
             I2S_BCLK,
             I2S_WS,
             I2S_DOUT,
             I2S_DIN,
             PA_ENABLE_GPIO,
             OUT_VOLUME);
    return ESP_OK;
}

void audio_chime_start(void)
{
    g_playing = true;
}

void audio_chime_stop(void)
{
    g_playing = false;
}

bool audio_chime_available(void)
{
    return g_available;
}

bool audio_chime_is_playing(void)
{
    return g_playing;
}

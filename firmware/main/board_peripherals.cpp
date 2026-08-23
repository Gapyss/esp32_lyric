#include "board_peripherals.h"

#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board_peripherals";
static const i2c_port_num_t I2C_PORT = I2C_NUM_0;
static const gpio_num_t I2C_SDA = GPIO_NUM_13;
static const gpio_num_t I2C_SCL = GPIO_NUM_14;
static const uint32_t I2C_FREQ_HZ = 400000;
static const uint8_t PCF85063_ADDR = 0x51;
static const uint8_t SHTC3_ADDR = 0x70;

// VBAT sense: GPIO4 / ADC1 channel 3, ~3:1 divider ahead of the ADC pin
// (see spec.md "ADC -- battery voltage").
static const adc_unit_t VBAT_ADC_UNIT = ADC_UNIT_1;
static const adc_channel_t VBAT_ADC_CHANNEL = ADC_CHANNEL_3;
static const adc_atten_t VBAT_ADC_ATTEN = ADC_ATTEN_DB_12;
static const int VBAT_DIVIDER_NUM = 3;

// History is sampled at most this often, independent of which screen is
// active, so the stats trend has data as soon as it's opened.
static const int64_t HISTORY_SAMPLE_INTERVAL_US = 60LL * 1000000LL;

// Charge curve for the single 18650 Li-ion cell in the holder, ordered full to
// empty and interpolated between points. The pack voltage is a poor gauge on
// its own -- it barely moves between 80% and 40% -- so the curve exists mainly
// to linearise the reading before the runtime estimate differentiates it.
static const struct {
    int mv;
    int percent;
} VBAT_CURVE[] = {
    {4200, 100}, {4100, 92}, {4000, 83}, {3900, 71}, {3830, 60}, {3790, 50}, {3760, 40},
    {3730, 30},  {3690, 20}, {3630, 12}, {3540, 6},  {3420, 3},  {3200, 0},
};

// Raw ADC reads swing by tens of millivolts between frames; this smooths them
// before anything downstream looks at the value.
static const float VBAT_SMOOTHING = 0.125f;

// Runtime is estimated by fitting a line through the recent charge level and
// extrapolating it to empty, which sidesteps having to know the pack capacity
// or the current draw (both of which swing with WiFi and backlight anyway).
#define BATTERY_TREND_CAPACITY 60
static const int64_t BATTERY_TREND_INTERVAL_US = 60LL * 1000000LL;
static const int64_t BATTERY_TREND_MIN_SPAN_US = 15LL * 60LL * 1000000LL;
// Past this much history with no visible movement, the drain really is too
// slow to name -- report STEADY rather than leaving "estimating" up forever.
static const int64_t BATTERY_TREND_SETTLED_SPAN_US = 45LL * 60LL * 1000000LL;
// In the flat middle of the curve one percent of charge is only ~3.5 mV, which
// is the same order as the residual ADC noise after smoothing. So the fit also
// waits for the charge level to have visibly *moved* -- elapsed time alone
// would happily fit a line through pure noise and report a confident number.
// This self-adjusts: a heavy drain qualifies in minutes, a light one waits.
static const float BATTERY_MIN_TREND_CHANGE_PCT = 3.0f;
// Below this the drain is lost in the noise of the fit, so it reports STEADY
// rather than an implausible multi-day figure.
static const float BATTERY_MIN_DRAIN_PCT_PER_HOUR = 2.0f;
static const int BATTERY_MAX_RUNTIME_MINUTES = BOARD_BATTERY_RUNTIME_CAP_MINUTES;

static i2c_master_bus_handle_t g_i2c_bus;
static i2c_master_dev_handle_t g_rtc_dev;
static i2c_master_dev_handle_t g_shtc3_dev;

static adc_oneshot_unit_handle_t g_adc_handle;
static adc_cali_handle_t g_adc_cali_handle;
static bool g_adc_cali_ok;

// Battery state is only ever touched from board_peripherals_read(), which the
// single render task drives, so unlike the env history it needs no mutex.
static float g_battery_mv_smoothed;
static bool g_battery_smoothed_valid;
static int64_t g_battery_trend_us[BATTERY_TREND_CAPACITY];
static float g_battery_trend_percent[BATTERY_TREND_CAPACITY];
static int g_battery_trend_count;
static int g_battery_trend_head;
static int64_t g_last_battery_trend_us;

static SemaphoreHandle_t g_history_mutex;
static int g_history_temp[BOARD_HISTORY_CAPACITY];
static int g_history_humidity[BOARD_HISTORY_CAPACITY];
static int g_history_count;
static int g_history_head;
static int64_t g_last_history_us;

static uint8_t bcd_to_int(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t int_to_bcd(int value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
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
    if (g_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_PORT;
        bus_cfg.sda_io_num = I2C_SDA;
        bus_cfg.scl_io_num = I2C_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
        if (err == ESP_ERR_INVALID_STATE) {
            err = i2c_master_get_bus_handle(I2C_PORT, &g_i2c_bus);
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = I2C_FREQ_HZ;

    if (g_rtc_dev == NULL) {
        dev_cfg.device_address = PCF85063_ADDR;
        esp_err_t err = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_rtc_dev);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (g_shtc3_dev == NULL) {
        dev_cfg.device_address = SHTC3_ADDR;
        esp_err_t err = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_shtc3_dev);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "I2C idle peripherals ready on SDA=%d SCL=%d", I2C_SDA, I2C_SCL);

    if (g_adc_handle == NULL) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {};
        unit_cfg.unit_id = VBAT_ADC_UNIT;
        esp_err_t adc_err = adc_oneshot_new_unit(&unit_cfg, &g_adc_handle);
        if (adc_err == ESP_OK) {
            adc_oneshot_chan_cfg_t chan_cfg = {};
            chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
            chan_cfg.atten = VBAT_ADC_ATTEN;
            adc_err = adc_oneshot_config_channel(g_adc_handle, VBAT_ADC_CHANNEL, &chan_cfg);
        }
        if (adc_err == ESP_OK) {
            adc_cali_curve_fitting_config_t cali_cfg = {};
            cali_cfg.unit_id = VBAT_ADC_UNIT;
            cali_cfg.chan = VBAT_ADC_CHANNEL;
            cali_cfg.atten = VBAT_ADC_ATTEN;
            cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
            g_adc_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_adc_cali_handle) == ESP_OK;
            ESP_LOGI(TAG, "VBAT ADC ready (calibrated=%d)", g_adc_cali_ok);
        } else {
            ESP_LOGW(TAG, "VBAT ADC unavailable: %s", esp_err_to_name(adc_err));
        }
    }

    return ESP_OK;
}

i2c_master_bus_handle_t board_peripherals_i2c_bus(void)
{
    return g_i2c_bus;
}

static void read_rtc(BoardIdleMetrics *metrics)
{
    uint8_t reg = 0x04;
    uint8_t data[3] = {};
    esp_err_t err = i2c_master_transmit_receive(g_rtc_dev, &reg, 1, data, sizeof(data), 50);
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
    i2c_master_transmit(g_shtc3_dev, data, sizeof(data), 50);
}

static void read_shtc3(BoardIdleMetrics *metrics)
{
    shtc3_command(0x3517); // wake
    vTaskDelay(pdMS_TO_TICKS(1));
    shtc3_command(0x7866); // normal power, clock stretching disabled, T first
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6] = {};
    esp_err_t err = i2c_master_receive(g_shtc3_dev, data, sizeof(data), 50);
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

static float battery_percent_from_mv(float mv)
{
    const int points = (int)(sizeof(VBAT_CURVE) / sizeof(VBAT_CURVE[0]));
    if (mv >= (float)VBAT_CURVE[0].mv) {
        return 100.0f;
    }
    for (int i = 1; i < points; i++) {
        if (mv >= (float)VBAT_CURVE[i].mv) {
            const float span_mv = (float)(VBAT_CURVE[i - 1].mv - VBAT_CURVE[i].mv);
            const float span_pct = (float)(VBAT_CURVE[i - 1].percent - VBAT_CURVE[i].percent);
            return (float)VBAT_CURVE[i].percent + span_pct * (mv - (float)VBAT_CURVE[i].mv) / span_mv;
        }
    }
    return 0.0f;
}

static void push_battery_trend(int64_t now_us, float percent)
{
    if (g_last_battery_trend_us != 0 && now_us - g_last_battery_trend_us < BATTERY_TREND_INTERVAL_US) {
        return;
    }
    g_last_battery_trend_us = now_us;

    int idx;
    if (g_battery_trend_count < BATTERY_TREND_CAPACITY) {
        idx = (g_battery_trend_head + g_battery_trend_count) % BATTERY_TREND_CAPACITY;
        g_battery_trend_count++;
    } else {
        idx = g_battery_trend_head;
        g_battery_trend_head = (g_battery_trend_head + 1) % BATTERY_TREND_CAPACITY;
    }
    g_battery_trend_us[idx] = now_us;
    g_battery_trend_percent[idx] = percent;
}

// Classifies the recent charge trend by least-squares fitting charge level
// against wall time. On a measurable drain the slope (percent per hour,
// negative) lands in `slope_out`. The samples carry real timestamps rather
// than being assumed one-a-minute, because screens that never call
// board_peripherals_read() (comic, apod) leave gaps in the series whenever
// they are on top.
static BoardBatteryTrend battery_classify_trend(float *slope_out)
{
    if (g_battery_trend_count < 3) {
        return BOARD_BATTERY_TREND_UNKNOWN;
    }
    const int oldest = g_battery_trend_head;
    const int newest = (g_battery_trend_head + g_battery_trend_count - 1) % BATTERY_TREND_CAPACITY;
    const int64_t span_us = g_battery_trend_us[newest] - g_battery_trend_us[oldest];
    if (span_us < BATTERY_TREND_MIN_SPAN_US) {
        return BOARD_BATTERY_TREND_UNKNOWN;
    }

    const float change = g_battery_trend_percent[newest] - g_battery_trend_percent[oldest];
    if (change > -BATTERY_MIN_TREND_CHANGE_PCT && change < BATTERY_MIN_TREND_CHANGE_PCT) {
        // The level hasn't visibly moved. Only call that steady once the window
        // is long enough that a drain worth naming would have surfaced by now;
        // before that it's still just warming up.
        return span_us >= BATTERY_TREND_SETTLED_SPAN_US ? BOARD_BATTERY_TREND_STEADY
                                                        : BOARD_BATTERY_TREND_UNKNOWN;
    }

    const double base_us = (double)g_battery_trend_us[oldest];
    double sum_h = 0.0, sum_p = 0.0, sum_hh = 0.0, sum_hp = 0.0;
    for (int i = 0; i < g_battery_trend_count; i++) {
        const int idx = (g_battery_trend_head + i) % BATTERY_TREND_CAPACITY;
        const double hours = ((double)g_battery_trend_us[idx] - base_us) / 3600000000.0;
        const double percent = (double)g_battery_trend_percent[idx];
        sum_h += hours;
        sum_p += percent;
        sum_hh += hours * hours;
        sum_hp += hours * percent;
    }
    const double n = (double)g_battery_trend_count;
    const double denom = n * sum_hh - sum_h * sum_h;
    if (denom <= 0.0) {
        return BOARD_BATTERY_TREND_UNKNOWN;
    }
    const float slope = (float)((n * sum_hp - sum_h * sum_p) / denom);
    if (slope > BATTERY_MIN_DRAIN_PCT_PER_HOUR) {
        return BOARD_BATTERY_TREND_CHARGING;
    }
    if (slope < -BATTERY_MIN_DRAIN_PCT_PER_HOUR) {
        *slope_out = slope;
        return BOARD_BATTERY_TREND_DISCHARGING;
    }
    return BOARD_BATTERY_TREND_STEADY;
}

static void read_battery(BoardIdleMetrics *metrics)
{
    if (g_adc_handle == NULL) {
        metrics->battery_valid = false;
        return;
    }
    int raw = 0;
    if (adc_oneshot_read(g_adc_handle, VBAT_ADC_CHANNEL, &raw) != ESP_OK) {
        metrics->battery_valid = false;
        return;
    }
    int pin_mv = 0;
    int pack_mv;
    if (g_adc_cali_ok && adc_cali_raw_to_voltage(g_adc_cali_handle, raw, &pin_mv) == ESP_OK) {
        pack_mv = pin_mv * VBAT_DIVIDER_NUM;
    } else {
        pack_mv = (raw * 3300 / 4095) * VBAT_DIVIDER_NUM;
    }

    if (!g_battery_smoothed_valid) {
        g_battery_mv_smoothed = (float)pack_mv;
        g_battery_smoothed_valid = true;
    } else {
        g_battery_mv_smoothed += VBAT_SMOOTHING * ((float)pack_mv - g_battery_mv_smoothed);
    }

    const float percent = battery_percent_from_mv(g_battery_mv_smoothed);
    metrics->battery_mv = (int)(g_battery_mv_smoothed + 0.5f);
    metrics->battery_percent = (int)(percent + 0.5f);
    metrics->battery_valid = true;

    push_battery_trend(esp_timer_get_time(), percent);

    float slope = 0.0f;
    metrics->battery_trend = battery_classify_trend(&slope);
    if (metrics->battery_trend == BOARD_BATTERY_TREND_DISCHARGING) {
        int minutes = (int)(percent / -slope * 60.0f + 0.5f);
        if (minutes > BATTERY_MAX_RUNTIME_MINUTES) {
            minutes = BATTERY_MAX_RUNTIME_MINUTES;
        }
        metrics->battery_runtime_minutes = minutes;
    }
}

static void push_history_locked(int temperature_c_x10, int humidity_x10)
{
    int idx;
    if (g_history_count < BOARD_HISTORY_CAPACITY) {
        idx = (g_history_head + g_history_count) % BOARD_HISTORY_CAPACITY;
        g_history_count++;
    } else {
        idx = g_history_head;
        g_history_head = (g_history_head + 1) % BOARD_HISTORY_CAPACITY;
    }
    g_history_temp[idx] = temperature_c_x10;
    g_history_humidity[idx] = humidity_x10;
}

static void sample_history_if_due(const BoardIdleMetrics *metrics)
{
    if (!metrics->env_valid) {
        return;
    }
    if (g_history_mutex == NULL) {
        g_history_mutex = xSemaphoreCreateMutex();
    }
    const int64_t now_us = esp_timer_get_time();
    if (g_last_history_us != 0 && now_us - g_last_history_us < HISTORY_SAMPLE_INTERVAL_US) {
        return;
    }
    g_last_history_us = now_us;
    xSemaphoreTake(g_history_mutex, portMAX_DELAY);
    push_history_locked(metrics->temperature_c_x10, metrics->humidity_x10);
    xSemaphoreGive(g_history_mutex);
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
    read_battery(metrics);
    sample_history_if_due(metrics);
}

void board_peripherals_get_history(BoardHistory *history)
{
    if (history == NULL) {
        return;
    }
    memset(history, 0, sizeof(*history));
    if (g_history_mutex == NULL) {
        return;
    }
    xSemaphoreTake(g_history_mutex, portMAX_DELAY);
    history->count = g_history_count;
    for (int i = 0; i < g_history_count; i++) {
        const int idx = (g_history_head + i) % BOARD_HISTORY_CAPACITY;
        history->temperature_c_x10[i] = g_history_temp[idx];
        history->humidity_x10[i] = g_history_humidity[idx];
    }
    xSemaphoreGive(g_history_mutex);
}

esp_err_t board_peripherals_set_rtc_from_local_time(const struct tm *local)
{
    if (local == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[8] = {
        0x04,
        int_to_bcd(local->tm_sec),
        int_to_bcd(local->tm_min),
        int_to_bcd(local->tm_hour),
        int_to_bcd(local->tm_mday),
        int_to_bcd(local->tm_wday),
        int_to_bcd(local->tm_mon + 1),
        int_to_bcd((local->tm_year + 1900) % 100),
    };
    return i2c_master_transmit(g_rtc_dev, data, sizeof(data), 50);
}

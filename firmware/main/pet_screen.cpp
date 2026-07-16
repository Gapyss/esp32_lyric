#include "pet_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "board_peripherals.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "pet_screen";

// Live sensor values refresh at this cadence, same contract as clock_screen.
static const int64_t LIVE_REFRESH_INTERVAL_US = 1000000LL;

// System clock is only meaningful once SNTP has set it (mirrors the guard in
// board_peripherals read_system_time and water_screen).
static const time_t CLOCK_VALID_EPOCH = 1700000000;

// Mood thresholds against SHTC3 readings (x10 fixed point).
static const int HOT_TEMP_C_X10 = 320;
static const int COLD_TEMP_C_X10 = 180;
static const int STICKY_HUMIDITY_X10 = 780;

// Sleeping window from the RTC clock: 22:00 .. 07:00.
static const int SLEEP_START_HOUR = 22;
static const int SLEEP_END_HOUR = 7;

// How long a pet keeps the creature in its happy state.
static const int64_t PETTED_GLOW_US = 4000000LL;

typedef enum {
    PET_MOOD_COZY = 0,
    PET_MOOD_SLEEPING,
    PET_MOOD_MELTING,
    PET_MOOD_FREEZING,
    PET_MOOD_STICKY,
    PET_MOOD_HAPPY,
} PetMood;

typedef struct {
    int64_t last_read_us;
    BoardIdleMetrics metrics;
    int64_t last_pet_us;
    int pets_today;
    int pets_total;
    long pets_day_marker;
    long adopt_day;  // days since epoch when first seen with a valid clock; 0 = unknown
} PetCtx;

static PetCtx g_pet;

static void load_state(void)
{
    nvs_handle_t handle;
    if (nvs_open(PET_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    int32_t value = 0;
    if (nvs_get_i32(handle, PET_NVS_TOTAL_KEY, &value) == ESP_OK) {
        g_pet.pets_total = value;
    }
    if (nvs_get_i32(handle, PET_NVS_ADOPT_KEY, &value) == ESP_OK) {
        g_pet.adopt_day = value;
    }
    nvs_close(handle);
}

static void save_i32(const char *key, int32_t value)
{
    nvs_handle_t handle;
    if (nvs_open(PET_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_i32(handle, key, value) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

// Same day-boundary derivation as water_screen: system clock, set once via
// SNTP; marker 0 means "day unknown yet".
static long current_day_marker(void)
{
    time_t now = 0;
    time(&now);
    if (now < CLOCK_VALID_EPOCH) {
        return 0;
    }
    struct tm local = {};
    if (localtime_r(&now, &local) == NULL) {
        return 0;
    }
    return (long)(local.tm_year * 10000 + local.tm_mon * 100 + local.tm_mday);
}

static long current_epoch_day(void)
{
    time_t now = 0;
    time(&now);
    if (now < CLOCK_VALID_EPOCH) {
        return 0;
    }
    return (long)(now / 86400);
}

static void refresh_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_pet.last_read_us != 0 && now_us - g_pet.last_read_us < LIVE_REFRESH_INTERVAL_US) {
        return;
    }
    g_pet.last_read_us = now_us;
    board_peripherals_read(&g_pet.metrics);

    const long marker = current_day_marker();
    if (marker != 0 && marker != g_pet.pets_day_marker) {
        g_pet.pets_day_marker = marker;
        g_pet.pets_today = 0;
    }

    // Adoption day is recorded the first time the creature is seen with a
    // valid clock, then never changes -- it is where AGE comes from.
    if (g_pet.adopt_day == 0) {
        const long today = current_epoch_day();
        if (today != 0) {
            g_pet.adopt_day = today;
            save_i32(PET_NVS_ADOPT_KEY, (int32_t)today);
            ESP_LOGI(TAG, "creature adopted, epoch day %ld", today);
        }
    }
}

void pet_screen_init(void)
{
    memset(&g_pet, 0, sizeof(g_pet));
    load_state();
}

void pet_pet(void)
{
    g_pet.last_pet_us = esp_timer_get_time();
    g_pet.pets_today++;
    g_pet.pets_total++;
    save_i32(PET_NVS_TOTAL_KEY, g_pet.pets_total);
    ESP_LOGI(TAG, "creature petted, today=%d total=%d", g_pet.pets_today, g_pet.pets_total);
}

static PetMood resolve_mood(int64_t now_us)
{
    if (g_pet.last_pet_us != 0 && now_us - g_pet.last_pet_us < PETTED_GLOW_US) {
        return PET_MOOD_HAPPY;  // petting wakes it, even at night
    }
    if (g_pet.metrics.time_valid &&
        (g_pet.metrics.hour >= SLEEP_START_HOUR || g_pet.metrics.hour < SLEEP_END_HOUR)) {
        return PET_MOOD_SLEEPING;
    }
    if (g_pet.metrics.env_valid) {
        if (g_pet.metrics.temperature_c_x10 >= HOT_TEMP_C_X10) {
            return PET_MOOD_MELTING;
        }
        if (g_pet.metrics.temperature_c_x10 <= COLD_TEMP_C_X10) {
            return PET_MOOD_FREEZING;
        }
        if (g_pet.metrics.humidity_x10 >= STICKY_HUMIDITY_X10) {
            return PET_MOOD_STICKY;
        }
    }
    return PET_MOOD_COZY;
}

static const char *mood_chip_text(PetMood mood)
{
    switch (mood) {
        case PET_MOOD_SLEEPING:
            return "SLEEPING";
        case PET_MOOD_MELTING:
            return "MELTING";
        case PET_MOOD_FREEZING:
            return "FREEZING";
        case PET_MOOD_STICKY:
            return "STICKY";
        case PET_MOOD_HAPPY:
            return "HAPPY";
        default:
            return "COZY";
    }
}

// Deadpan status line in the Tend voice: lowercase, dot separator, no drama.
// Rotates every ~7 seconds within the current mood's pool.
static void format_quip(char *out, size_t out_len, PetMood mood, int64_t now_us, long age_days)
{
    const int slot = (int)(now_us / 7000000LL);
    const int t_whole = g_pet.metrics.temperature_c_x10 / 10;
    const int t_frac = g_pet.metrics.temperature_c_x10 < 0 ? -(g_pet.metrics.temperature_c_x10 % 10)
                                                           : g_pet.metrics.temperature_c_x10 % 10;
    const int rh = g_pet.metrics.humidity_x10 / 10;

    switch (mood) {
        case PET_MOOD_SLEEPING:
            switch (slot % 3) {
                case 0:
                    snprintf(out, out_len, "do not disturb - recharging");
                    return;
                case 1:
                    snprintf(out, out_len, "dreaming in 1-bit");
                    return;
                default:
                    snprintf(out, out_len, "night mode - even creatures need sleep");
                    return;
            }
        case PET_MOOD_MELTING:
            switch (slot % 3) {
                case 0:
                    snprintf(out, out_len, "%d.%d c - i am basically soup", t_whole, t_frac);
                    return;
                case 1:
                    snprintf(out, out_len, "have you considered a fan - asking for me");
                    return;
                default:
                    snprintf(out, out_len, "my thermal paste is sweating");
                    return;
            }
        case PET_MOOD_FREEZING:
            switch (slot % 3) {
                case 0:
                    snprintf(out, out_len, "%d.%d c - my solder joints are chilly", t_whole, t_frac);
                    return;
                case 1:
                    snprintf(out, out_len, "shivering at 240 mhz");
                    return;
                default:
                    snprintf(out, out_len, "a warm drink would help - for you - i sip volts");
                    return;
            }
        case PET_MOOD_STICKY:
            if (slot % 2 == 0) {
                snprintf(out, out_len, "%d%% humidity - my pixels feel damp", rh);
            } else {
                snprintf(out, out_len, "it is not the heat - it is the humidity");
            }
            return;
        case PET_MOOD_HAPPY:
            switch (slot % 3) {
                case 0:
                    snprintf(out, out_len, "pet received - affection buffer full");
                    return;
                case 1:
                    snprintf(out, out_len, "%d pets today - a personal best probably", g_pet.pets_today);
                    return;
                default:
                    snprintf(out, out_len, "logging warmth - thank you");
                    return;
            }
        default:
            switch (slot % 4) {
                case 0:
                    snprintf(out, out_len, "all conditions nominal - i am thriving");
                    return;
                case 1:
                    if (age_days > 0) {
                        snprintf(out, out_len, "day %ld of guarding this desk", age_days);
                    } else {
                        snprintf(out, out_len, "guarding this desk since boot");
                    }
                    return;
                case 2:
                    snprintf(out, out_len, "i blink so you remember to");
                    return;
                default:
                    snprintf(out, out_len, "powered by wifi and quiet ambition");
                    return;
            }
    }
}

static void draw_centered(u8g2_t *u8, const char *text, int y)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, (width - text_width) / 2, y, text);
}

// Same panel-polarity note as the other screens: draw color 1 is Tend paper
// (light/reflective), 0 is Tend ink. Per-screen duplication of this helper is
// the convention in this firmware.
static void begin_light_theme(u8g2_t *u8)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, width, height);
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);
}

static void draw_heart(u8g2_t *u8, int cx, int cy, int r)
{
    u8g2_DrawDisc(u8, cx - r, cy, r, U8G2_DRAW_ALL);
    u8g2_DrawDisc(u8, cx + r, cy, r, U8G2_DRAW_ALL);
    u8g2_DrawTriangle(u8, cx - 2 * r, cy, cx + 2 * r + 1, cy, cx, cy + 2 * r + r / 2);
}

static void draw_sweat_drop(u8g2_t *u8, int cx, int cy)
{
    u8g2_DrawDisc(u8, cx, cy + 2, 3, U8G2_DRAW_ALL);
    u8g2_DrawTriangle(u8, cx - 3, cy + 2, cx + 3, cy + 2, cx, cy - 4);
}

// The creature itself: a rounded blob with an ember-flame antenna, eyes, a
// mood-specific mouth, and stubby feet. All coordinates hang off the blob
// center (cx, cy) so the idle bob/shiver offsets move everything together.
static void draw_creature(u8g2_t *u8, int cx, int cy, PetMood mood, int64_t now_us)
{
    const int64_t now_ms = now_us / 1000;

    // Body outline, doubled for weight (u8g2 frames are 1px).
    const int body_w = 120;
    const int body_h = 96;
    u8g2_DrawRFrame(u8, cx - body_w / 2, cy - body_h / 2, body_w, body_h, 40);
    u8g2_DrawRFrame(u8, cx - body_w / 2 + 1, cy - body_h / 2 + 1, body_w - 2, body_h - 2, 39);

    // Feet.
    u8g2_DrawRBox(u8, cx - 36, cy + body_h / 2 - 4, 22, 10, 4);
    u8g2_DrawRBox(u8, cx + 14, cy + body_h / 2 - 4, 22, 10, 4);

    // Antenna with an ember flame that flickers; sleeping snuffs it to a stub.
    const int head_y = cy - body_h / 2;
    u8g2_DrawLine(u8, cx, head_y, cx, head_y - 10);
    u8g2_DrawLine(u8, cx + 1, head_y, cx + 1, head_y - 10);
    if (mood != PET_MOOD_SLEEPING) {
        const int flicker = (int)((now_ms / 220) % 2);
        u8g2_DrawTriangle(u8,
                          cx - 4 - flicker,
                          head_y - 9,
                          cx + 5 + flicker,
                          head_y - 9,
                          cx,
                          head_y - 20 - flicker);
    }

    // Eyes: blink briefly on a fixed cycle; pupils drift side to side slowly.
    const int look = (int)lroundf(2.0f * sinf((float)(now_ms % 12000) * (float)M_PI * 2.0f / 12000.0f));
    const int eye_dx = 24;
    const int eye_y = cy - 12;
    const bool blinking = (now_ms % 3400) < 140;
    for (int side = -1; side <= 1; side += 2) {
        const int ex = cx + side * eye_dx + look;
        if (mood == PET_MOOD_SLEEPING) {
            u8g2_DrawCircle(u8, ex, eye_y - 2, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
        } else if (mood == PET_MOOD_HAPPY) {
            u8g2_DrawCircle(u8, ex, eye_y + 3, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
            u8g2_DrawCircle(u8, ex, eye_y + 3, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
        } else if (blinking) {
            u8g2_DrawHLine(u8, ex - 5, eye_y, 11);
        } else {
            u8g2_DrawDisc(u8, ex, eye_y, 5, U8G2_DRAW_ALL);
        }
    }

    // Blush marks when happy: two short strokes under each eye.
    if (mood == PET_MOOD_HAPPY) {
        for (int side = -1; side <= 1; side += 2) {
            const int bx = cx + side * (eye_dx + 8);
            u8g2_DrawLine(u8, bx - 3, eye_y + 12, bx + 3, eye_y + 14);
            u8g2_DrawLine(u8, bx - 3, eye_y + 16, bx + 3, eye_y + 18);
        }
    }

    // Mouth.
    const int mouth_y = cy + 16;
    switch (mood) {
        case PET_MOOD_SLEEPING:
            u8g2_DrawCircle(u8, cx, mouth_y + 2, 3, U8G2_DRAW_ALL);
            break;
        case PET_MOOD_MELTING:
            u8g2_DrawDisc(u8, cx, mouth_y + 2, 5, U8G2_DRAW_ALL);
            break;
        case PET_MOOD_FREEZING:
            for (int i = 0; i < 4; i++) {
                const int x0 = cx - 12 + i * 6;
                const int y0 = (i % 2 == 0) ? mouth_y + 3 : mouth_y;
                const int y1 = (i % 2 == 0) ? mouth_y : mouth_y + 3;
                u8g2_DrawLine(u8, x0, y0, x0 + 6, y1);
            }
            break;
        case PET_MOOD_STICKY:
            u8g2_DrawCircle(u8, cx, mouth_y + 8, 8, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
            break;
        case PET_MOOD_HAPPY:
            u8g2_DrawCircle(u8, cx, mouth_y - 4, 11, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
            u8g2_DrawCircle(u8, cx, mouth_y - 4, 10, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
            break;
        default:
            u8g2_DrawCircle(u8, cx, mouth_y - 3, 8, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
            break;
    }

    // Mood dressing around the blob.
    if (mood == PET_MOOD_MELTING) {
        // Sweat drops sliding down beside the head on a short loop.
        const int fall = (int)((now_ms % 1200) * 22 / 1200);
        draw_sweat_drop(u8, cx - body_w / 2 - 12, cy - 28 + fall);
        draw_sweat_drop(u8, cx + body_w / 2 + 12, cy - 18 + fall);
    } else if (mood == PET_MOOD_SLEEPING) {
        // Drifting z z z above the head.
        const int drift = (int)((now_ms % 3000) * 10 / 3000);
        u8g2_SetFont(u8, u8g2_font_6x12_tf);
        u8g2_DrawUTF8(u8, cx + 34, head_y - 8 - drift, "z");
        u8g2_DrawUTF8(u8, cx + 44, head_y - 18 - drift, "z");
        u8g2_SetFont(u8, u8g2_font_helvB10_tf);
        u8g2_DrawUTF8(u8, cx + 54, head_y - 30 - drift, "z");
    } else if (mood == PET_MOOD_HAPPY) {
        // Hearts float up and away for the duration of the glow.
        const int rise = (int)((now_us - g_pet.last_pet_us) * 46 / PETTED_GLOW_US);
        draw_heart(u8, cx - body_w / 2 - 18, cy - 10 - rise, 4);
        draw_heart(u8, cx + body_w / 2 + 16, cy - 2 - rise * 3 / 4, 3);
        draw_heart(u8, cx + body_w / 2 + 32, cy - 26 - rise / 2, 2);
    }
}

static void draw_stat_card(u8g2_t *u8, int x, int y, int w, int h, const char *label, const char *value)
{
    u8g2_DrawRFrame(u8, x, y, w, h, 8);
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, x + 10, y + 13, label);
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, x + 10, y + 30, value);
}

void pet_render_current(u8g2_t *u8)
{
    refresh_if_due();

    const int64_t now_us = esp_timer_get_time();
    const int64_t now_ms = now_us / 1000;
    const PetMood mood = resolve_mood(now_us);

    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int m = 14;

    // --- header: eyebrow + name left, mood chip + clock right ---
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, m, 20, "DESK CREATURE");
    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    u8g2_DrawUTF8(u8, m, 50, "EMBER");

    // Ember accent as an inverted ink chip, same convention as clock_screen.
    const char *chip = mood_chip_text(mood);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    const int chip_tw = (int)u8g2_GetUTF8Width(u8, chip);
    const int chip_w = chip_tw + 16;
    const int chip_h = 18;
    const int chip_x = width - m - chip_w;
    const int chip_y = 12;
    u8g2_DrawRBox(u8, chip_x, chip_y, chip_w, chip_h, 8);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawUTF8(u8, chip_x + 8, chip_y + 13, chip);
    u8g2_SetDrawColor(u8, 0);

    char clock[8];
    if (g_pet.metrics.time_valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", g_pet.metrics.hour, g_pet.metrics.minute);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    const int clock_w = (int)u8g2_GetUTF8Width(u8, clock);
    u8g2_DrawUTF8(u8, width - m - clock_w, chip_y + chip_h + 12, clock);

    u8g2_DrawHLine(u8, m, 58, width - 2 * m);

    // --- body: bobbing (or shivering) creature over a ground shadow ---
    const int cx_base = width / 2;
    const int cy_base = 144;
    int bob = (int)lroundf(3.0f * sinf((float)(now_ms % 2400) * (float)M_PI * 2.0f / 2400.0f));
    int jitter = 0;
    if (mood == PET_MOOD_SLEEPING) {
        bob = (int)lroundf(1.0f * sinf((float)(now_ms % 3600) * (float)M_PI * 2.0f / 3600.0f));
    } else if (mood == PET_MOOD_FREEZING) {
        jitter = ((now_ms / 90) % 2 == 0) ? 1 : -1;
    }
    u8g2_DrawEllipse(u8, cx_base, cy_base + 58, 44, 5, U8G2_DRAW_ALL);
    draw_creature(u8, cx_base + jitter, cy_base + bob, mood, now_us);

    // --- quip line ---
    const long age_days = g_pet.adopt_day > 0 ? current_epoch_day() - g_pet.adopt_day + 1 : 0;
    char quip[64];
    format_quip(quip, sizeof(quip), mood, now_us, age_days);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, quip, 232);

    // --- stat cards: age, pets, current comfort readings ---
    const int card_y = 244;
    const int card_h = 36;
    const int card_w = 118;
    const int gap = 9;

    char age_text[16];
    if (age_days > 0) {
        snprintf(age_text, sizeof(age_text), "%ld DAY%s", age_days, age_days == 1 ? "" : "S");
    } else {
        snprintf(age_text, sizeof(age_text), "--");
    }
    char pets_text[20];
    snprintf(pets_text, sizeof(pets_text), "%d / %d", g_pet.pets_today, g_pet.pets_total);
    char comfort_text[32];
    if (g_pet.metrics.env_valid) {
        snprintf(comfort_text,
                 sizeof(comfort_text),
                 "%d.%dC %d%%",
                 g_pet.metrics.temperature_c_x10 / 10,
                 g_pet.metrics.temperature_c_x10 < 0 ? -(g_pet.metrics.temperature_c_x10 % 10)
                                                     : g_pet.metrics.temperature_c_x10 % 10,
                 g_pet.metrics.humidity_x10 / 10);
    } else {
        snprintf(comfort_text, sizeof(comfort_text), "--");
    }

    draw_stat_card(u8, m, card_y, card_w, card_h, "AGE", age_text);
    draw_stat_card(u8, m + card_w + gap, card_y, card_w, card_h, "PETS TODAY/ALL", pets_text);
    draw_stat_card(u8, m + 2 * (card_w + gap), card_y, card_w, card_h, "HABITAT", comfort_text);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    draw_centered(u8, "BTN: PET  |  BOOT: MODE  |  g4pys.company", 294);
}

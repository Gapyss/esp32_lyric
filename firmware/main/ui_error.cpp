#include "ui_error.h"

#include <string.h>

#include "esp_timer.h"

static const int64_t UI_ERROR_DURATION_US = 3 * 1000000LL;

static char g_message[48];
static int64_t g_deadline_us;

void ui_error_show(const char *message)
{
    if (message == NULL) {
        return;
    }
    strncpy(g_message, message, sizeof(g_message) - 1);
    g_message[sizeof(g_message) - 1] = '\0';
    g_deadline_us = esp_timer_get_time() + UI_ERROR_DURATION_US;
}

bool ui_error_render(u8g2_t *u8g2)
{
    if (g_deadline_us == 0 || esp_timer_get_time() >= g_deadline_us) {
        return false;
    }

    const int width = (int)u8g2_GetDisplayWidth(u8g2);
    const int height = (int)u8g2_GetDisplayHeight(u8g2);
    const int band_h = 28;
    const int band_y = height - band_h - 4;

    u8g2_DrawFrame(u8g2, 4, band_y, width - 8, band_h);
    u8g2_SetFont(u8g2, u8g2_font_helvB14_tf);
    const int text_width = (int)u8g2_GetUTF8Width(u8g2, g_message);
    u8g2_DrawUTF8(u8g2, (width - text_width) / 2, band_y + band_h - 8, g_message);
    return true;
}

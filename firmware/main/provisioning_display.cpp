#include "provisioning_display.h"

#include <stdio.h>
#include <string.h>

#include "network_manager.h"
#include "qrcode.h"

namespace {

constexpr int MAX_QR_SIDE = 57;  // Version 10.
bool g_qr_modules[MAX_QR_SIDE * MAX_QR_SIDE];
int g_qr_size;
char g_qr_key[64];

void center_text(u8g2_t *u8, const char *text, int baseline)
{
    const int width = u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, ((int)u8g2_GetDisplayWidth(u8) - width) / 2, baseline, text);
}

void capture_qr(esp_qrcode_handle_t qr, void *)
{
    const int size = esp_qrcode_get_size(qr);
    if (size <= 0 || size > MAX_QR_SIDE) {
        g_qr_size = 0;
        return;
    }
    g_qr_size = size;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            g_qr_modules[y * MAX_QR_SIDE + x] = esp_qrcode_get_module(qr, x, y);
        }
    }
}

void ensure_qr(const NetworkSnapshot &snapshot)
{
    char key[64];
    snprintf(key, sizeof(key), "%s|%s", snapshot.setup_ssid, snapshot.setup_password);
    if (strcmp(key, g_qr_key) == 0 && g_qr_size > 0) return;

    char payload[96];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
             snapshot.setup_ssid, snapshot.setup_password);
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func_with_cb = capture_qr;
    config.user_data = nullptr;
    config.max_qrcode_version = 10;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    g_qr_size = 0;
    if (esp_qrcode_generate(&config, payload) == ESP_OK && g_qr_size > 0) {
        snprintf(g_qr_key, sizeof(g_qr_key), "%s", key);
    }
}

void draw_qr(u8g2_t *u8)
{
    if (g_qr_size <= 0) return;
    constexpr int region_x = 8;
    constexpr int region_y = 52;
    constexpr int region_size = 190;
    const int scale = region_size / (g_qr_size + 8);
    const int total = (g_qr_size + 8) * scale;
    const int origin_x = region_x + (region_size - total) / 2 + 4 * scale;
    const int origin_y = region_y + (region_size - total) / 2 + 4 * scale;
    u8g2_SetDrawColor(u8, 1);
    for (int y = 0; y < g_qr_size; ++y) {
        for (int x = 0; x < g_qr_size; ++x) {
            if (g_qr_modules[y * MAX_QR_SIDE + x]) {
                u8g2_DrawBox(u8, origin_x + x * scale, origin_y + y * scale, scale, scale);
            }
        }
    }
}

void render_handoff(u8g2_t *u8, const NetworkSnapshot &snapshot)
{
    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    center_text(u8, "Wi-Fi connected", 92);
    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    center_text(u8, snapshot.station_ip, 145);
    char hostname[56];
    snprintf(hostname, sizeof(hostname), "%s.local", snapshot.hostname);
    center_text(u8, hostname, 183);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    center_text(u8, "Starting normal services...", 235);
}

}  // namespace

extern "C" void provisioning_display_render(u8g2_t *u8)
{
    NetworkSnapshot snapshot = {};
    network_manager_get_snapshot(&snapshot);
    u8g2_SetFontMode(u8, 1);
    u8g2_SetDrawColor(u8, 1);

    if (snapshot.state == NETWORK_STATE_HANDOFF) {
        render_handoff(u8, snapshot);
        return;
    }

    ensure_qr(snapshot);
    draw_qr(u8);
    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    u8g2_DrawUTF8(u8, 12, 28, snapshot.manual_setup ? "Wi-Fi setup" : "Connect this display");
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, 214, 72, "NETWORK");
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, 214, 94, snapshot.setup_ssid);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, 214, 124, "PASSWORD");
    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    u8g2_DrawUTF8(u8, 214, 149, snapshot.setup_password);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, 214, 181, "OPEN IN A BROWSER");
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, 214, 204, "http://192.168.4.1");

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    const char *message = snapshot.error[0] != '\0' ? snapshot.error : snapshot.status;
    u8g2_DrawUTF8(u8, 12, 278, message);
    if (snapshot.reset_seconds_remaining > 0) {
        char countdown[48];
        snprintf(countdown, sizeof(countdown), "RESET NETWORK IN %d", snapshot.reset_seconds_remaining);
        u8g2_SetFont(u8, u8g2_font_helvB14_tf);
        const int width = u8g2_GetUTF8Width(u8, countdown);
        u8g2_SetDrawColor(u8, 0);
        u8g2_DrawBox(u8, 205, 220, 190, 38);
        u8g2_SetDrawColor(u8, 1);
        u8g2_DrawFrame(u8, 205, 220, 190, 38);
        u8g2_DrawUTF8(u8, 300 - width / 2, 246, countdown);
    }
}

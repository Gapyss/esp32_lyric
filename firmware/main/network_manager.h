#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETWORK_STATE_UNPROVISIONED = 0,
    NETWORK_STATE_BOOT_CONNECTING,
    NETWORK_STATE_ONLINE,
    NETWORK_STATE_RECONNECTING,
    NETWORK_STATE_AUTO_PROVISIONING,
    NETWORK_STATE_MANUAL_PROVISIONING,
    NETWORK_STATE_VALIDATING,
    NETWORK_STATE_HANDOFF,
    NETWORK_STATE_NETWORK_RESET,
} NetworkState;

typedef enum {
    PROVISIONING_AUTH_OPEN = 0,
    PROVISIONING_AUTH_WPA2 = 1,
    PROVISIONING_AUTH_WPA2_WPA3 = 2,
    PROVISIONING_AUTH_UNSUPPORTED = 255,
} ProvisioningAuth;

typedef struct {
    char ssid[33];
    int8_t rssi;
    ProvisioningAuth auth;
    bool supported;
} ProvisioningNetwork;

typedef struct {
    NetworkState state;
    bool has_saved_network;
    bool portal_active;
    bool manual_setup;
    bool scanning;
    bool validating;
    bool can_cancel;
    int reset_seconds_remaining;
    char setup_ssid[33];
    char setup_password[13];
    char hostname[40];
    char station_ip[16];
    char status[64];
    char error[96];
} NetworkSnapshot;

typedef enum {
    NETWORK_SERVICES_PAUSE = 0,
    NETWORK_SERVICES_ONLINE,
} NetworkServiceAction;

typedef void (*network_service_callback_t)(NetworkServiceAction action,
                                           const NetworkSnapshot *snapshot,
                                           void *context);

esp_err_t network_manager_start(network_service_callback_t callback, void *context);
void network_manager_get_snapshot(NetworkSnapshot *snapshot);
size_t network_manager_get_scan_results(ProvisioningNetwork *results, size_t capacity);

esp_err_t network_manager_request_scan(void);
esp_err_t network_manager_submit_candidate(const char *ssid,
                                           const char *password,
                                           ProvisioningAuth auth);
esp_err_t network_manager_cancel_setup(void);
esp_err_t network_manager_open_manual_setup(void);
esp_err_t network_manager_reset_provisioning(void);
void network_manager_set_reset_countdown(int seconds_remaining);

bool network_manager_provisioning_display_active(void);
const char *network_state_name(NetworkState state);

#ifdef __cplusplus
}
#endif

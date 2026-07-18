#include "network_manager.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "provisioning_portal.h"

namespace {

constexpr char TAG[] = "network_manager";
constexpr char NVS_NAMESPACE[] = "wifi_prov";
constexpr char NVS_ACTIVE_KEY[] = "active";
constexpr char NVS_AP_PASS_KEY[] = "ap_pass";
constexpr uint32_t RECORD_MAGIC = 0x46573447;  // "G4WF"
constexpr uint8_t RECORD_VERSION = 1;
constexpr int64_t BOOT_TIMEOUT_US = 30LL * 1000 * 1000;
constexpr int64_t VALIDATION_TIMEOUT_US = 30LL * 1000 * 1000;
constexpr int64_t RECONNECT_TIMEOUT_US = 5LL * 60 * 1000 * 1000;
constexpr int64_t MANUAL_IDLE_TIMEOUT_US = 10LL * 60 * 1000 * 1000;
constexpr int64_t HANDOFF_DELAY_US = 10LL * 1000 * 1000;
constexpr int64_t BACKGROUND_RETRY_US = 10LL * 1000 * 1000;
constexpr size_t MAX_SCAN_RESULTS = 24;

struct WifiRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t auth;
    uint8_t ssid_len;
    uint8_t password_len;
    uint8_t ssid[32];
    uint8_t password[64];
    uint32_t checksum;
};

enum class EventType : uint8_t {
    StaStarted,
    StaConnected,
    Disconnected,
    GotIp,
    ScanDone,
    RequestScan,
    SubmitCandidate,
    Cancel,
    ManualSetup,
    NetworkReset,
};

struct ManagerEvent {
    EventType type;
    uint8_t disconnect_reason;
    char ip[16];
    char ssid[33];
    char password[64];
    ProvisioningAuth auth;
};

QueueHandle_t g_queue;
SemaphoreHandle_t g_mutex;
NetworkSnapshot g_snapshot;
ProvisioningNetwork g_scan_results[MAX_SCAN_RESULTS];
size_t g_scan_count;
WifiRecord g_active_record;
WifiRecord g_candidate_record;
bool g_has_active;
bool g_sta_has_ip;
bool g_ignore_next_disconnect;
bool g_services_announced;
bool g_candidate_submission_pending;
int64_t g_deadline_us;
int64_t g_next_retry_us;
NetworkState g_validation_return_state;
network_service_callback_t g_service_callback;
void *g_service_context;
esp_netif_t *g_ap_netif;

uint32_t record_checksum(const WifiRecord &record)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(WifiRecord, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

bool auth_supported(ProvisioningAuth auth)
{
    return auth == PROVISIONING_AUTH_OPEN || auth == PROVISIONING_AUTH_WPA2 ||
           auth == PROVISIONING_AUTH_WPA2_WPA3;
}

bool record_valid(const WifiRecord &record)
{
    if (record.magic != RECORD_MAGIC || record.version != RECORD_VERSION ||
        record.ssid_len == 0 || record.ssid_len > sizeof(record.ssid) ||
        record.password_len > 63 || !auth_supported(static_cast<ProvisioningAuth>(record.auth)) ||
        record.checksum != record_checksum(record)) {
        return false;
    }
    if (record.auth == PROVISIONING_AUTH_OPEN) {
        return record.password_len == 0;
    }
    return record.password_len >= 8;
}

WifiRecord make_record(const char *ssid, const char *password, ProvisioningAuth auth)
{
    WifiRecord record = {};
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    record.auth = static_cast<uint8_t>(auth);
    record.ssid_len = static_cast<uint8_t>(strlen(ssid));
    record.password_len = static_cast<uint8_t>(strlen(password));
    memcpy(record.ssid, ssid, record.ssid_len);
    memcpy(record.password, password, record.password_len);
    record.checksum = record_checksum(record);
    return record;
}

void snapshot_update_state(NetworkState state, const char *status, const char *error = nullptr)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot.state = state;
    g_snapshot.validating = state == NETWORK_STATE_VALIDATING;
    snprintf(g_snapshot.status, sizeof(g_snapshot.status), "%s", status == nullptr ? "" : status);
    snprintf(g_snapshot.error, sizeof(g_snapshot.error), "%s", error == nullptr ? "" : error);
    xSemaphoreGive(g_mutex);
}

void snapshot_set_portal(bool active, bool manual)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot.portal_active = active;
    g_snapshot.manual_setup = active && manual;
    g_snapshot.can_cancel = active && manual && g_has_active;
    xSemaphoreGive(g_mutex);
}

void snapshot_set_ip(const char *ip)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    snprintf(g_snapshot.station_ip, sizeof(g_snapshot.station_ip), "%s", ip == nullptr ? "" : ip);
    xSemaphoreGive(g_mutex);
}

NetworkSnapshot snapshot_copy()
{
    NetworkSnapshot copy;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    copy = g_snapshot;
    xSemaphoreGive(g_mutex);
    return copy;
}

void notify_services(NetworkServiceAction action)
{
    if (action == NETWORK_SERVICES_PAUSE && !g_services_announced) {
        return;
    }
    if (action == NETWORK_SERVICES_ONLINE && g_services_announced) {
        return;
    }
    g_services_announced = action == NETWORK_SERVICES_ONLINE;
    if (g_service_callback != nullptr) {
        const NetworkSnapshot copy = snapshot_copy();
        g_service_callback(action, &copy, g_service_context);
    }
}

void set_has_active(bool has_active)
{
    g_has_active = has_active;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot.has_saved_network = has_active;
    g_snapshot.can_cancel = g_snapshot.portal_active && g_snapshot.manual_setup && has_active;
    xSemaphoreGive(g_mutex);
}

esp_err_t load_active_record()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(g_active_record);
    err = nvs_get_blob(handle, NVS_ACTIVE_KEY, &g_active_record, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(g_active_record) || !record_valid(g_active_record)) {
        memset(&g_active_record, 0, sizeof(g_active_record));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t save_active_record(const WifiRecord &record)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_ACTIVE_KEY, &record, sizeof(record));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return err;
}

void generate_setup_password(char output[13])
{
    static constexpr char ALPHABET[] = "2346789ABCDEFGHJKLMNPQRTUVWXYZ";
    for (size_t i = 0; i < 12; ++i) {
        output[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    }
    output[12] = '\0';
}

esp_err_t load_or_create_setup_password(bool force_new)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char password[13] = {};
    size_t length = sizeof(password);
    if (force_new || nvs_get_str(handle, NVS_AP_PASS_KEY, password, &length) != ESP_OK ||
        strlen(password) != 12) {
        generate_setup_password(password);
        err = nvs_set_str(handle, NVS_AP_PASS_KEY, password);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        snprintf(g_snapshot.setup_password, sizeof(g_snapshot.setup_password), "%s", password);
        xSemaphoreGive(g_mutex);
    }
    return err;
}

esp_err_t erase_provisioning_namespace()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return err;
}

wifi_auth_mode_t wifi_auth_for(ProvisioningAuth auth)
{
    switch (auth) {
        case PROVISIONING_AUTH_OPEN:
            return WIFI_AUTH_OPEN;
        case PROVISIONING_AUTH_WPA2_WPA3:
            return WIFI_AUTH_WPA2_WPA3_PSK;
        case PROVISIONING_AUTH_WPA2:
        default:
            return WIFI_AUTH_WPA2_PSK;
    }
}

esp_err_t configure_station(const WifiRecord &record)
{
    wifi_config_t config = {};
    memcpy(config.sta.ssid, record.ssid, record.ssid_len);
    memcpy(config.sta.password, record.password, record.password_len);
    config.sta.threshold.authmode = wifi_auth_for(static_cast<ProvisioningAuth>(record.auth));
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t configure_setup_ap()
{
    const NetworkSnapshot copy = snapshot_copy();
    wifi_config_t config = {};
    const size_t setup_ssid_len = strnlen(copy.setup_ssid, sizeof(config.ap.ssid));
    memcpy(config.ap.ssid, copy.setup_ssid, setup_ssid_len);
    snprintf(reinterpret_cast<char *>(config.ap.password), sizeof(config.ap.password), "%s", copy.setup_password);
    config.ap.ssid_len = setup_ssid_len;
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_ip_info_t ip = {};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(g_ap_netif);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(g_ap_netif, &ip));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(g_ap_netif));
    return ESP_OK;
}

void stop_portal()
{
    provisioning_portal_stop();
    snapshot_set_portal(false, false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
}

void start_portal(NetworkState state, bool manual, const char *status)
{
    notify_services(NETWORK_SERVICES_PAUSE);
    snapshot_update_state(state, status);
    snapshot_set_portal(true, manual);
    if (configure_setup_ap() != ESP_OK || provisioning_portal_start() != ESP_OK) {
        snapshot_update_state(state, "Setup unavailable", "Could not start setup portal");
        return;
    }
    ManagerEvent scan = {};
    scan.type = EventType::RequestScan;
    xQueueSend(g_queue, &scan, 0);
}

void connect_record(const WifiRecord &record)
{
    g_sta_has_ip = false;
    snapshot_set_ip("");
    ESP_ERROR_CHECK_WITHOUT_ABORT(configure_station(record));
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "station connect request failed: %s", esp_err_to_name(err));
    }
}

void disconnect_for_reconfigure()
{
    const esp_err_t err = esp_wifi_disconnect();
    g_ignore_next_disconnect = err == ESP_OK;
}

void transition_online(const char *ip)
{
    g_sta_has_ip = true;
    snapshot_set_ip(ip);
    if (provisioning_portal_is_active()) {
        stop_portal();
    }
    snapshot_update_state(NETWORK_STATE_ONLINE, "Online");
    notify_services(NETWORK_SERVICES_ONLINE);
}

const char *safe_disconnect_message(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "Authentication failed; check the password";
        case WIFI_REASON_NO_AP_FOUND:
            return "Network not found";
        case WIFI_REASON_ASSOC_FAIL:
            return "The network rejected the connection";
        default:
            return "Could not connect to that network";
    }
}

void restore_after_validation_failure(const char *message)
{
    const NetworkState return_state = g_validation_return_state;
    snapshot_update_state(return_state, "Ready to try again", message);
    if (g_has_active) {
        disconnect_for_reconfigure();
        connect_record(g_active_record);
    }
    g_deadline_us = 0;
    g_next_retry_us = esp_timer_get_time() + BACKGROUND_RETRY_US;
}

void refresh_scan_results()
{
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        count = 0;
    }
    if (count > MAX_SCAN_RESULTS) {
        count = MAX_SCAN_RESULTS;
    }
    wifi_ap_record_t records[MAX_SCAN_RESULTS] = {};
    uint16_t fetched = count;
    if (fetched > 0 && esp_wifi_scan_get_ap_records(&fetched, records) != ESP_OK) {
        fetched = 0;
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_scan_count = fetched;
    for (size_t i = 0; i < fetched; ++i) {
        ProvisioningNetwork &result = g_scan_results[i];
        memset(&result, 0, sizeof(result));
        memcpy(result.ssid, records[i].ssid, sizeof(records[i].ssid));
        result.ssid[32] = '\0';
        result.rssi = records[i].rssi;
        if (records[i].authmode == WIFI_AUTH_OPEN) {
            result.auth = PROVISIONING_AUTH_OPEN;
        } else if (records[i].authmode == WIFI_AUTH_WPA2_PSK) {
            result.auth = PROVISIONING_AUTH_WPA2;
        } else if (records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK) {
            result.auth = PROVISIONING_AUTH_WPA2_WPA3;
        } else {
            result.auth = PROVISIONING_AUTH_UNSUPPORTED;
        }
        result.supported = auth_supported(result.auth);
    }
    g_snapshot.scanning = false;
    xSemaphoreGive(g_mutex);
}

void begin_scan()
{
    const NetworkSnapshot copy = snapshot_copy();
    if (!copy.portal_active || copy.validating || copy.scanning) {
        return;
    }
    wifi_scan_config_t config = {};
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&config, false) == ESP_OK) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_snapshot.scanning = true;
        xSemaphoreGive(g_mutex);
    }
}

void begin_validation(const ManagerEvent &event)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_candidate_submission_pending = false;
    xSemaphoreGive(g_mutex);
    const NetworkSnapshot copy = snapshot_copy();
    if (!copy.portal_active || copy.validating) {
        return;
    }
    g_candidate_record = make_record(event.ssid, event.password, event.auth);
    g_validation_return_state = copy.manual_setup ? NETWORK_STATE_MANUAL_PROVISIONING
                                                 : (g_has_active ? NETWORK_STATE_AUTO_PROVISIONING
                                                                 : NETWORK_STATE_UNPROVISIONED);
    snapshot_update_state(NETWORK_STATE_VALIDATING, "Connecting to candidate network");
    g_deadline_us = esp_timer_get_time() + VALIDATION_TIMEOUT_US;
    disconnect_for_reconfigure();
    connect_record(g_candidate_record);
}

void handle_got_ip(const ManagerEvent &event)
{
    const NetworkSnapshot copy = snapshot_copy();
    g_sta_has_ip = true;
    snapshot_set_ip(event.ip);
    if (copy.state == NETWORK_STATE_VALIDATING) {
        const esp_err_t err = save_active_record(g_candidate_record);
        if (err != ESP_OK) {
            restore_after_validation_failure("Connected, but credentials could not be saved");
            return;
        }
        g_active_record = g_candidate_record;
        set_has_active(true);
        snapshot_update_state(NETWORK_STATE_HANDOFF, "Setup complete");
        g_deadline_us = esp_timer_get_time() + HANDOFF_DELAY_US;
        return;
    }
    if (copy.state == NETWORK_STATE_MANUAL_PROVISIONING) {
        snapshot_update_state(NETWORK_STATE_MANUAL_PROVISIONING, "Saved network is online");
        return;
    }
    transition_online(event.ip);
}

void handle_disconnect(uint8_t reason)
{
    g_sta_has_ip = false;
    snapshot_set_ip("");
    if (g_ignore_next_disconnect) {
        g_ignore_next_disconnect = false;
        return;
    }

    const NetworkSnapshot copy = snapshot_copy();
    if (copy.state == NETWORK_STATE_VALIDATING) {
        restore_after_validation_failure(safe_disconnect_message(reason));
        return;
    }
    if (copy.state == NETWORK_STATE_ONLINE) {
        snapshot_update_state(NETWORK_STATE_RECONNECTING, "Reconnecting");
        g_deadline_us = esp_timer_get_time() + RECONNECT_TIMEOUT_US;
        g_next_retry_us = 0;
    }
    if (g_has_active && !copy.validating) {
        esp_wifi_connect();
    }
}

void close_manual_setup()
{
    const NetworkSnapshot before = snapshot_copy();
    const bool was_connected = g_sta_has_ip;
    stop_portal();
    if (g_has_active) {
        if (was_connected) {
            transition_online(before.station_ip);
            return;
        }
        disconnect_for_reconfigure();
        connect_record(g_active_record);
        snapshot_update_state(NETWORK_STATE_RECONNECTING, "Reconnecting");
        g_deadline_us = esp_timer_get_time() + RECONNECT_TIMEOUT_US;
        notify_services(NETWORK_SERVICES_ONLINE);
    } else {
        start_portal(NETWORK_STATE_UNPROVISIONED, false, "Choose a Wi-Fi network");
    }
}

void handle_event(const ManagerEvent &event)
{
    const int64_t now = esp_timer_get_time();
    switch (event.type) {
        case EventType::StaStarted:
            if (g_has_active) {
                connect_record(g_active_record);
            }
            break;
        case EventType::StaConnected: {
            const NetworkSnapshot copy = snapshot_copy();
            if (copy.state == NETWORK_STATE_VALIDATING) {
                snapshot_update_state(NETWORK_STATE_VALIDATING, "Waiting for an IP address");
            }
            break;
        }
        case EventType::Disconnected:
            handle_disconnect(event.disconnect_reason);
            break;
        case EventType::GotIp:
            handle_got_ip(event);
            break;
        case EventType::ScanDone:
            refresh_scan_results();
            break;
        case EventType::RequestScan:
            begin_scan();
            break;
        case EventType::SubmitCandidate:
            begin_validation(event);
            break;
        case EventType::Cancel: {
            const NetworkSnapshot copy = snapshot_copy();
            if (copy.manual_setup && g_has_active) {
                close_manual_setup();
            }
            break;
        }
        case EventType::ManualSetup: {
            const NetworkSnapshot copy = snapshot_copy();
            if (!copy.portal_active) {
                start_portal(NETWORK_STATE_MANUAL_PROVISIONING, true, "Manual setup");
            } else if (!copy.manual_setup && g_has_active) {
                snapshot_update_state(NETWORK_STATE_MANUAL_PROVISIONING, "Manual setup");
                snapshot_set_portal(true, true);
            }
            break;
        }
        case EventType::NetworkReset:
            notify_services(NETWORK_SERVICES_PAUSE);
            snapshot_update_state(NETWORK_STATE_NETWORK_RESET, "Resetting network settings");
            provisioning_portal_stop();
            erase_provisioning_namespace();
            memset(&g_active_record, 0, sizeof(g_active_record));
            set_has_active(false);
            load_or_create_setup_password(true);
            disconnect_for_reconfigure();
            start_portal(NETWORK_STATE_UNPROVISIONED, false, "Choose a Wi-Fi network");
            break;
    }
    (void)now;
}

void handle_deadlines()
{
    const int64_t now = esp_timer_get_time();
    const NetworkSnapshot copy = snapshot_copy();

    if (copy.state == NETWORK_STATE_BOOT_CONNECTING && now >= g_deadline_us) {
        start_portal(NETWORK_STATE_AUTO_PROVISIONING, false, "Saved network unavailable");
        g_next_retry_us = now + BACKGROUND_RETRY_US;
    } else if (copy.state == NETWORK_STATE_RECONNECTING && now >= g_deadline_us) {
        start_portal(NETWORK_STATE_AUTO_PROVISIONING, false, "Connection recovery setup");
        g_next_retry_us = now + BACKGROUND_RETRY_US;
    } else if (copy.state == NETWORK_STATE_VALIDATING && now >= g_deadline_us) {
        restore_after_validation_failure("Connection timed out");
    } else if (copy.state == NETWORK_STATE_HANDOFF && now >= g_deadline_us) {
        transition_online(copy.station_ip);
    }

    const NetworkSnapshot refreshed = snapshot_copy();
    if (refreshed.state == NETWORK_STATE_AUTO_PROVISIONING && g_has_active && now >= g_next_retry_us) {
        connect_record(g_active_record);
        g_next_retry_us = now + BACKGROUND_RETRY_US;
    }
    if (refreshed.state == NETWORK_STATE_MANUAL_PROVISIONING && provisioning_portal_is_active()) {
        const int64_t activity = provisioning_portal_last_activity_us();
        if (activity > 0 && now - activity >= MANUAL_IDLE_TIMEOUT_US) {
            close_manual_setup();
        }
    }
}

void manager_task(void *)
{
    while (true) {
        ManagerEvent event = {};
        if (xQueueReceive(g_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_event(event);
        }
        handle_deadlines();
    }
}

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (g_queue == nullptr) {
        return;
    }
    ManagerEvent event = {};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        event.type = EventType::StaStarted;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        event.type = EventType::StaConnected;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        event.type = EventType::Disconnected;
        event.disconnect_reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        event.type = EventType::ScanDone;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        event.type = EventType::GotIp;
        const auto *got_ip = static_cast<ip_event_got_ip_t *>(data);
        snprintf(event.ip, sizeof(event.ip), IPSTR, IP2STR(&got_ip->ip_info.ip));
    } else {
        return;
    }
    xQueueSend(g_queue, &event, 0);
}

esp_err_t enqueue_simple(EventType type)
{
    if (g_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ManagerEvent event = {};
    event.type = type;
    return xQueueSend(g_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

}  // namespace

extern "C" esp_err_t network_manager_start(network_service_callback_t callback, void *context)
{
    if (g_queue != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(12, sizeof(ManagerEvent));
    if (g_mutex == nullptr || g_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_service_callback = callback;
    g_service_context = context;
    memset(&g_snapshot, 0, sizeof(g_snapshot));

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read station MAC");
    snprintf(g_snapshot.setup_ssid, sizeof(g_snapshot.setup_ssid), "G4PYS-Setup-%02X%02X", mac[4], mac[5]);
    snprintf(g_snapshot.hostname, sizeof(g_snapshot.hostname), "g4pys-company-%02x%02x", mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(load_or_create_setup_password(false), TAG, "load setup password");
    set_has_active(load_active_record() == ESP_OK);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    if (g_ap_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi RAM storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, nullptr, nullptr),
                        TAG, "Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, nullptr, nullptr),
                        TAG, "IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");

    if (g_has_active) {
        snapshot_update_state(NETWORK_STATE_BOOT_CONNECTING, "Connecting to saved network");
        g_deadline_us = esp_timer_get_time() + BOOT_TIMEOUT_US;
    } else {
        start_portal(NETWORK_STATE_UNPROVISIONED, false, "Choose a Wi-Fi network");
    }

    const BaseType_t task_ok = xTaskCreatePinnedToCore(manager_task, "network_manager", 8192,
                                                       nullptr, 6, nullptr, 0);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" void network_manager_get_snapshot(NetworkSnapshot *snapshot)
{
    if (snapshot == nullptr || g_mutex == nullptr) {
        return;
    }
    *snapshot = snapshot_copy();
}

extern "C" size_t network_manager_get_scan_results(ProvisioningNetwork *results, size_t capacity)
{
    if (results == nullptr || capacity == 0 || g_mutex == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const size_t count = g_scan_count < capacity ? g_scan_count : capacity;
    memcpy(results, g_scan_results, count * sizeof(*results));
    xSemaphoreGive(g_mutex);
    return count;
}

extern "C" esp_err_t network_manager_request_scan(void)
{
    const NetworkSnapshot copy = snapshot_copy();
    if (!copy.portal_active || copy.validating || copy.scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_simple(EventType::RequestScan);
}

extern "C" esp_err_t network_manager_submit_candidate(const char *ssid,
                                                        const char *password,
                                                        ProvisioningAuth auth)
{
    if (ssid == nullptr || password == nullptr || !auth_supported(auth)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ssid_len = strlen(ssid);
    const size_t password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 63 ||
        (auth == PROVISIONING_AUTH_OPEN && password_len != 0) ||
        (auth != PROVISIONING_AUTH_OPEN && password_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_snapshot.validating || g_candidate_submission_pending) {
        xSemaphoreGive(g_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    g_candidate_submission_pending = true;
    xSemaphoreGive(g_mutex);
    ManagerEvent event = {};
    event.type = EventType::SubmitCandidate;
    event.auth = auth;
    memcpy(event.ssid, ssid, ssid_len + 1);
    memcpy(event.password, password, password_len + 1);
    if (xQueueSend(g_queue, &event, 0) == pdTRUE) {
        return ESP_OK;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_candidate_submission_pending = false;
    xSemaphoreGive(g_mutex);
    return ESP_ERR_TIMEOUT;
}

extern "C" esp_err_t network_manager_cancel_setup(void)
{
    return enqueue_simple(EventType::Cancel);
}

extern "C" esp_err_t network_manager_open_manual_setup(void)
{
    return enqueue_simple(EventType::ManualSetup);
}

extern "C" esp_err_t network_manager_reset_provisioning(void)
{
    return enqueue_simple(EventType::NetworkReset);
}

extern "C" void network_manager_set_reset_countdown(int seconds_remaining)
{
    if (g_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot.reset_seconds_remaining = seconds_remaining;
    xSemaphoreGive(g_mutex);
}

extern "C" bool network_manager_provisioning_display_active(void)
{
    if (g_mutex == nullptr) {
        return false;
    }
    const NetworkSnapshot copy = snapshot_copy();
    return copy.portal_active || copy.state == NETWORK_STATE_HANDOFF ||
           copy.state == NETWORK_STATE_NETWORK_RESET;
}

extern "C" const char *network_state_name(NetworkState state)
{
    switch (state) {
        case NETWORK_STATE_UNPROVISIONED: return "unprovisioned";
        case NETWORK_STATE_BOOT_CONNECTING: return "boot_connecting";
        case NETWORK_STATE_ONLINE: return "online";
        case NETWORK_STATE_RECONNECTING: return "reconnecting";
        case NETWORK_STATE_AUTO_PROVISIONING: return "auto_provisioning";
        case NETWORK_STATE_MANUAL_PROVISIONING: return "manual_provisioning";
        case NETWORK_STATE_VALIDATING: return "validating";
        case NETWORK_STATE_HANDOFF: return "handoff";
        case NETWORK_STATE_NETWORK_RESET: return "network_reset";
        default: return "unknown";
    }
}

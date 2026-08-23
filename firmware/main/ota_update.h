#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Firmware update over WiFi. Loads (or creates on first boot) the token that
// guards the upload endpoint and logs it, so it is always recoverable over
// serial. Call once before http_api_start().
esp_err_t ota_update_init(void);

// Registers GET /ota (status, open like the rest of the API) and POST /ota
// (upload, token gated) on an already-started server.
esp_err_t ota_update_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

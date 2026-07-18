#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_CONNECTION_PAIRING_REQUIRED = 0,
    BOARD_CONNECTION_DISCOVERING,
    BOARD_CONNECTION_CONNECTING,
    BOARD_CONNECTION_AUTHENTICATING,
    BOARD_CONNECTION_CONNECTED,
    BOARD_CONNECTION_AUTH_FAILED,
    BOARD_CONNECTION_RETRY_WAIT,
} BoardConnectionState;

esp_err_t board_client_start(void);
void board_client_network_changed(bool online);
BoardConnectionState board_client_get_state(void);
const char *board_client_state_name(BoardConnectionState state);
bool board_client_has_pairing_token(void);
esp_err_t board_client_set_pairing_token(const char *token);

#ifdef __cplusplus
}
#endif

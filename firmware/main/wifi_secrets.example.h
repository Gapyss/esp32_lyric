#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// Optional fallback when mDNS discovery of _lyrics._tcp is not available.
// Example: #define LYRICS_DAEMON_HOST "192.168.1.25"
#define LYRICS_DAEMON_HOST "192.168.1.42"
#define LYRICS_DAEMON_PORT 8766

// Optional shared token for the daemon board WebSocket.
// Must match G4PYS_LYRICS_BOARD_TOKEN when that environment variable is set.
#define LYRICS_DAEMON_TOKEN ""

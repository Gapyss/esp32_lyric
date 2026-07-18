# G4PYS display firmware

For complete daemon and board pairing instructions, see
[Lyrics Board Pairing Guide](../docs/lyrics-board-pairing.md).

## Build and flash

The firmware targets ESP-IDF 5.5 or newer and no longer uses a local
`wifi_secrets.h` file.

```sh
idf.py build
idf.py flash monitor
```

## First-time Wi-Fi setup

After a fresh flash, the display shows a protected setup network named
`G4PYS-Setup-XXXX`, its generated password, a QR code, and the fallback URL
`http://192.168.4.1`. Join that network with a phone or computer and use the
captive portal to select a supported 2.4 GHz network. Credentials are saved
only after the board receives a DHCP address; successful setup continues
without a reboot.

The same portal accepts the lyrics pairing token. Generate/show it on the Mac:

```sh
python3 daemon/lyrics_display_daemon.py pairing-token
```

The daemon stores its UUID and token in `~/.g4pys/lyrics-identity.json` with
mode `0600`. The ESP32 stores only the token and the UUID of the last
authenticated daemon in NVS; it never stores a daemon IP. Leaving the token
blank is supported: Wi-Fi comes online and the lyrics connection displays
`PAIRING_REQUIRED`. An already-networked board can also be paired without
changing Wi-Fi:

```sh
curl -X POST --data 'token=PASTE_64_HEX_TOKEN' http://BOARD.local/pairing
```

To reconfigure Wi-Fi later, hold both board buttons for five seconds. Continue
holding until 15 seconds to erase only the Wi-Fi provisioning record and create
a new setup password. Releasing between five and 15 seconds leaves setup open
without erasing the saved network.

## Secure lyrics connection

After DHCP, the background client browses Bonjour `_lyrics._tcp` and accepts
secure services only when TXT contains `proto=2`, `auth=hmac-sha256`, and a
valid `uuid`. It prefers the remembered UUID and IPv4 addresses on the board's
subnet, then connects to the discovered SRV port at `/board`. Authentication
uses independent client/server nonces and HMAC-SHA256; HKDF derives separate
keys for each direction. All hello, ready, clear, and framebuffer records are
then SEC2 HMAC-protected with monotonically increasing sequence numbers.

Discovery is repeated after Wi-Fi changes, IP changes, disconnects, and daemon
restarts. Retries use exponential backoff with jitter (30-second maximum).
WebSocket pings are sent every 20 seconds and must receive a pong within 10
seconds. Legacy proto=1 is compiled out by default; migration builds must set
`LYRICS_ALLOW_LEGACY_PROTO1=1`, and discovery never downgrades automatically.

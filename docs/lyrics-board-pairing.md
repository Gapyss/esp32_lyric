# Lyrics Board Pairing Guide

This guide pairs an ESP32 lyrics board with the lyrics daemon running on a Mac.
The board stores the pairing token and preferred daemon UUID in NVS. It never
stores the Mac's IP address.

## 1. Start the daemon

On the Mac:

```sh
cd /Users/g4pys/esp-32
python3 daemon/lyrics_display_daemon.py serve
```

Keep this process running. A successful startup includes output similar to:

```text
board WebSocket: ws://0.0.0.0:8766/board
board WebSocket auth: mutual nonce/HMAC (proto=2)
mDNS advertisement: g4pys Lyrics Display._lyrics._tcp.local:8766
```

## 2. Get the pairing token

In another Mac terminal:

```sh
cd /Users/g4pys/esp-32
python3 daemon/lyrics_display_daemon.py pairing-token
```

Copy the complete 64-character hexadecimal value printed after
`pairing token:`. The daemon identity and token live in
`~/.g4pys/lyrics-identity.json`, which is created with `0600` permissions.

## 3A. Pair during first-time Wi-Fi setup

1. Join the board's `G4PYS-Setup-XXXX` Wi-Fi network.
2. Open `http://192.168.4.1`.
3. Select the 2.4 GHz Wi-Fi network and enter its password.
4. Paste the daemon pairing token into **Lyrics daemon pairing token**.
5. Press **Connect**.

Leaving the token blank does not prevent Wi-Fi setup. The board will connect to
Wi-Fi and display `PAIRING_REQUIRED` until it is paired later.

## 3B. Pair a board already connected to Wi-Fi

The board hostname uses the final four characters from its setup network. For
example, `G4PYS-Setup-536C` becomes:

```text
g4pys-company-536c.local
```

Submit the token:

```sh
curl -X POST \
  --data 'token=PASTE_64_HEX_TOKEN_HERE' \
  http://g4pys-company-536c.local/pairing
```

If `.local` resolution is unavailable, use the IP displayed on the board:

```sh
curl -X POST \
  --data 'token=PASTE_64_HEX_TOKEN_HERE' \
  http://192.168.1.37/pairing
```

## 4. Verify the connection

Query the board:

```sh
curl http://g4pys-company-536c.local/pairing
```

Successful output:

```json
{"paired":true,"state":"CONNECTED"}
```

The normal state sequence is:

```text
PAIRING_REQUIRED -> DISCOVERING -> CONNECTING -> AUTHENTICATING -> CONNECTED
                                      \-> AUTH_FAILED / RETRY_WAIT
```

The board retries discovery automatically with exponential backoff capped at
30 seconds.

## Troubleshooting

### `curl: (6) Could not resolve host`

Do not use the literal placeholder `BOARD.local`. Use the actual
`g4pys-company-xxxx.local` hostname or the board's displayed IPv4 address.

Discover the board hostname from the Mac with:

```sh
dns-sd -B _http._tcp local.
```

### Board shows `PAIRING_REQUIRED`

The board has Wi-Fi but no valid token. Run the `pairing-token` command and
submit the token through the setup portal or `/pairing` endpoint.

### Board shows `AUTH_FAILED`

The board and daemon tokens differ. Generate/show the current daemon token and
submit it to the board again. Authentication failure does not erase pairing.

### Board shows `RETRY_WAIT`

First confirm the daemon is still running:

```sh
lsof -nP -iTCP:8766 -sTCP:LISTEN
```

Then confirm Bonjour is visible:

```sh
dns-sd -B _lyrics._tcp local.
```

The service should appear as `g4pys Lyrics Display`. Resolve its security
metadata with:

```sh
dns-sd -L 'g4pys Lyrics Display' _lyrics._tcp local.
```

Expected TXT fields are `proto=2`, `auth=hmac-sha256`, and `uuid=<UUID>`.
Ensure the Mac and board are on the same LAN and that Wi-Fi client isolation is
disabled. After correcting the cause, allow up to 30 seconds for another retry.

### Restart without erasing pairing

Restart the ESP32 with its RESET button and restart the daemon with the `serve`
command. This preserves Wi-Fi and pairing. Do not hold both board buttons for
15 seconds unless the saved Wi-Fi configuration should also be erased.

## Legacy protocol

Secure proto=2 is the default. Proto=1 is disabled unless both sides are run as
an explicit migration configuration. A proto=2 authentication failure never
causes an automatic downgrade.

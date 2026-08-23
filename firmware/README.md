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

## Updating over Wi-Fi (OTA)

The flash carries two 6 MB app slots (`ota_0` / `ota_1`). The board runs one,
receives an update into the other, and reboots into it.

**One USB flash is required first.** A board running an older build has the old
single-`factory` partition table, and a partition table can only be replaced
over USB:

```sh
idf.py flash monitor
```

That migration keeps `nvs` at its original offset and size, so Wi-Fi
credentials and the pairing token survive it. Watch the boot log for the line
you will need afterwards:

```
I (…) ota_update: OTA upload token: 7KMQ2XPD4WHN
```

The token is generated once, stored in NVS, and reprinted at every boot. After
that, updates go over the network:

```sh
idf.py build
export G4PYS_OTA_TOKEN=7KMQ2XPD4WHN
tools/ota_flash.py --host g4pys-company-XXXX.local
```

The hostname is derived from the last two bytes of the board's MAC, and the
boot log prints it: `normal services available at g4pys-company-XXXX.local`.

The token can come from `G4PYS_OTA_TOKEN` as above or from `--token
7KMQ2XPD4WHN`; the flag wins if both are set. The tool checks the token,
uploads the image, waits for the reboot, and confirms which slot the board came
back on. `--status` reports the running slot and build time without uploading
anything and needs no token.

Under the hood, `POST /ota` takes the raw `.bin` as the body with the token in
an `X-OTA-Token` header. A partial or corrupt upload is rejected by
`esp_ota_end()` before anything is pointed at it, so a failed update leaves the
board running the image it already had.

There is **no automatic rollback**: if an uploaded image boots badly, recover
over USB. Rollback would make every boot depend on reaching a mark-valid call,
which turns any early-boot abort into a silent revert — a worse failure mode
for a board that has a USB port on the desk.

Note that `/ota` is the only authenticated endpoint. The rest of the HTTP API
(`/mode`, `/pairing`, `/diag/display`, …) is unauthenticated on the LAN, as
it was before. OTA is gated because arbitrary code execution is a different
severity class, not because the API as a whole is locked down.

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

## Battery and power behaviour

The board runs from an 18650 cell, so the firmware avoids waking the CPU
whenever it can rather than blanking the screen. The ST7305 is memory-in-pixel:
holding an image costs essentially nothing, and only *changing* it costs power.

- **Dynamic frequency scaling.** The CPU scales between 80 MHz and the
  configured boot frequency (160 MHz). It never boosts to the chip's 240 MHz
  ceiling, so this only ever scales down. `CONFIG_PM_ENABLE=y`; automatic light
  sleep is deliberately off, because the USB-Serial/JTAG console holds a power
  management lock while a host is attached. Boot logs `DFS enabled: 80-160 MHz`.
- **Per-screen render cadence.** Animated screens (music, sand)
  redraw every 70 ms. Everything else redraws at 1 Hz, which is already the
  resolution of what those screens display. Wi-Fi setup redraws at 200 ms.
- **Dirty-frame skip.** A drawn frame is compared against a shadow copy of what
  the panel already holds, and the 15 KB SPI transfer is skipped when they
  match. At 10 MHz that transfer is the single most expensive thing the render
  loop does.
- **Interrupt-driven buttons.** Both buttons wake a dedicated task from a GPIO
  edge interrupt. It polls at 20 ms only while a button is actually held, so an
  idle board has no button wakeups at all, and a press repaints immediately
  instead of waiting out a slow render period.
- **Wi-Fi modem sleep.** `WIFI_PS_MIN_MODEM`, not `MAX_MODEM`: the board holds a
  live WebSocket for pushed lyric frames, and the longer listen interval shows
  up as visible lyric lag.

To measure a change, read the drain figure on the **stats** screen. It needs to
run **off USB** — on USB the trend reads `CHARGING` and the runtime estimate
means nothing — and needs roughly 45 minutes to settle before the %/hour figure
is trustworthy.

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

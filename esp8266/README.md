# Clawdmeter — ESP8266 lyrics cube

## What is this?

A tiny desk display that shows **one thing**: the song you're playing, with
synced lyrics. Nothing else — no clock face, no pomodoro, no toys. It runs the
**Tend** look shared with the ESP32 firmware in this repo: warm paper, ink, one
ember accent.

It runs on the GeekMagic **HelloCubic Lite** / **SmallTV-Ultra** (an ESP8266
with an ST7789 240×240 color TFT), ported from
[Gapyss/clawdmeter-esp8266](https://github.com/Gapyss/clawdmeter-esp8266).
The device drives the LCD **and** serves a small web dashboard at the same URL,
which is where you tend it: it has no buttons.

```
lyrics daemon (lyrics_display_daemon.py --insecure)
   └── WebSocket :8766 ──1-bit UI + RGB565 cover art──> ESP8266 lyrics screen

claudemeter daemon (claudemeter_daemon.py)
   └── HTTP POST /usage + /nowplaying ──> ESP8266   (teaches it the Mac's IP)
```

### The screen

The lyrics screen is rendered **entirely on the Mac** by
`daemon/lyrics_display_daemon.py`: a Tend-color 240×240 UI with Core Text Thai
shaping, syllable karaoke, and real-color album covers, streamed to the panel
frame by frame. There is no on-device now-playing renderer.

When no frame is flowing — daemon off, Mac asleep, nothing playing — the panel
falls back to the **waiting screen**, which is deliberately useful rather than
blank:

```
🔥 lyrics                                 20:14:33
──────────────────────────────────────────────────

              waiting for lyrics

                 192.168.1.42
                clawdmeter.local

                wifi: my-network
```

That address is how you reach the dashboard from a phone without a serial cable
or a hunt through the router's client list. It repaints when the IP or network
changes, and turns into `wifi lost / reconnecting` in ember if the link drops.

## What you need

- A GeekMagic HelloCubic Lite / SmallTV-Ultra (or any ESP8266 + ST7789
  240×240 board — pins are set at the top of the sketch).
- A USB data cable for the **first** flash (updates after that are OTA).
- Arduino IDE or `arduino-cli` on your computer.
- A Mac on the same WiFi to run the daemons (see step 4).

## Setup

### 1. Flash the firmware (USB, first time only)

1. Open `esp8266/clawdmeter_esp8266/clawdmeter_esp8266.ino` in the Arduino IDE:
   - Boards Manager → install **esp8266 by ESP8266 Community**
   - Library Manager → install **WiFiManager** (tzapu) and
     **GFX Library for Arduino** (moononournation)
2. Select your board (e.g. NodeMCU 1.0 / Wemos D1 mini), set
   **Tools → MMU → 16KB cache + 48KB IRAM**, pick the port, then **Upload**.
   There are no WiFi credentials to edit in the code — see the next step.

Or from the CLI:

```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816 \
  --output-dir esp8266/bin esp8266/clawdmeter_esp8266
```

### 2. Connect it to WiFi (first boot, no code editing)

On first boot the device has no saved network, so it opens its own hotspot and
tells you so on the LCD:

```
🔥 wifi setup

        join this hotspot from a phone
             Clawdmeter-setup

          a setup page opens by itself
                if not, browse to
                  192.168.4.1

           waiting 3 min, then retrying
```

1. Join **Clawdmeter-setup** from your phone — a captive setup portal pops up.
2. Pick your WiFi, enter the password. The device saves it and reboots onto
   your network, then shows its new IP on the waiting screen.

The credentials persist across reboots and OTA updates. If nobody finishes the
portal within 3 minutes the device reboots and retries the saved network, so a
temporary router outage just heals itself.

**Automatic recovery:** if the device can't reach its saved network (new router,
new password, moved house), it reopens the **Clawdmeter-setup** hotspot by
itself on the next boot. No reflashing.

### 3. Changing WiFi or resetting the network on purpose

The dashboard's **network** card carries both resets, and the LCD shows you the
new address as soon as the device is back:

| Button | Endpoint | What it wipes |
|---|---|---|
| **change wifi** | `/wifi-reset` | The saved SSID/password only. Brightness and everything else survive. Reboots straight into the **Clawdmeter-setup** hotspot. |
| **reset settings** | `/factory-reset` | The saved WiFi **and** stored settings (brightness back to default). Also reboots into the hotspot. |

Reach for **change wifi** when you're just moving the box to a different
network; that's the whole point of it being separate.

Both are also plain `GET`s, so `curl http://clawdmeter.local/wifi-reset` works
if the dashboard is out of reach. They reset-and-reboot rather than opening the
portal in place: the portal blocks until someone finishes it, and running it
from inside a request handler would take the web server down with it.

### 4. Start the Mac daemons

The lyrics screen needs **two** processes on the Mac. Start order doesn't
matter.

```sh
# terminal 1 — renders the frames and serves this board on :8766
python3 daemon/lyrics_display_daemon.py serve --insecure

# terminal 2 — teaches the board where the Mac is (see below)
CLAWDMETER_DEVICE_URL=http://clawdmeter.local \
CLAWDMETER_DAILY_IMAGES=off \
python3 daemon/claudemeter_daemon.py
```

**Why the third one?** The board is never told the Mac's address — it *learns*
it from the source IP of a `/usage` or `/nowplaying` push, then dials back to
port 8766. `claudemeter_daemon.py` is what makes those pushes, so without it the
board sits on `waiting for lyrics` forever. Set `CLAWDMETER_DAILY_IMAGES=off`:
this firmware has no comic/APOD screen and no `/daily` endpoint, so leaving it
on just logs a failed push every 6 hours. Usage polling stays off by default and
there's no usage screen here, but the numbers still show on the dashboard if you
want them (`CLAWDMETER_USAGE_SOURCE=api`).

**Why `--insecure`?** By default the lyrics daemon creates an identity and
requires a mutual HMAC-SHA256 handshake (secure proto=2) — the scheme the ESP32
e-ink board uses. The ESP8266 lacks the heap to buffer-and-verify a whole SEC2
record on top of its framebuffer, so it speaks the unauthenticated proto=1
instead. `--insecure` drops the identity, advertises `proto=1`, and streams
unwrapped frames this board can render. (Or set `G4PYS_LYRICS_INSECURE=1`.)
Without it the board can't complete the handshake and the panel just stays on
the waiting screen.

There is no pairing step and no token on this link — the trade is LAN-link
authentication for ~7 KB of heap, so run it on a trusted home network.

> If you also run the ESP32 e-ink board, note that `--insecure` is process-wide
> and there is one daemon: the e-ink board finds it over mDNS and dials the same
> process, so turning the flag on for this box drops that board's authentication
> too. Splitting them is not currently an escape — a second daemon has no way to
> learn playback state, since the browser extension connects to one address.

> The lyrics daemon has no launchd plist wired up — restart it by hand after
> editing daemon code.

### 5. Play something

Load the browser extension (`browser_extension/` → `chrome://extensions` →
Developer mode → Load unpacked) and play a track in **YouTube Music**. It feeds
title/artist/position to the lyrics daemon.

Within a few seconds the panel switches from the waiting screen to the streamed
frames. **If it stays on the waiting screen**, the screen itself tells you which
half is missing:

| The panel says | What it means | Fix |
|---|---|---|
| `waiting for lyrics` | No Mac IP known yet, or the daemon is connected but idle between tracks | Start `claudemeter_daemon.py` (step 4) — that's what teaches the board the Mac's IP |
| `reaching the lyrics daemon…` | Mac IP known, the socket won't come up | Start `lyrics_display_daemon.py serve --insecure`; check nothing is blocking `:8766` between Mac and board |
| Streamed frames never appear, but the socket connects | The daemon is demanding a proto=2 handshake this board can't do | Restart it with `--insecure` — a bare `lyrics_display_daemon.py serve` won't talk to this board |

If the stream drops (Mac asleep, daemon stopped), the panel returns to the
waiting screen within seconds and reconnects on its own when the daemon comes
back.

### 6. Updating later (OTA, no USB)

The firmware serves an update page at `http://clawdmeter.local/update`.
Export a compiled `.bin` (Arduino IDE: **Sketch → Export Compiled Binary**, or
the `arduino-cli` command above — it lands in `esp8266/bin/`) and upload it
there; the device flashes and reboots on its own.

> The `/update` page has no password. On a trusted home LAN that's usually
> fine; to lock it, change `httpUpdater.setup(&server);` to
> `httpUpdater.setup(&server, "admin", "yourpassword");` and re-flash.

## Source layout

```
esp8266/
├── clawdmeter_esp8266/
│   ├── clawdmeter_esp8266.ino   # WiFi + HTTP API + dashboard + the waiting screen
│   ├── tend.h                   # shared Tend palette + the .ino/stream contract
│   ├── lyrics_stream.cpp        # the lyrics screen: WS client for the lyrics daemon
│   └── index_html_gz.h          # generated — gzipped dashboard served at /
└── tools/
    └── gen_index_gz.py          # regenerate index_html_gz.h after editing INDEX_HTML
```

## Device HTTP API

| Endpoint | Methods | Purpose |
|---|---|---|
| `/` | GET | Web dashboard (gzipped, from PROGMEM) |
| `/usage` | GET/POST | Daemon push. Draws nothing here — it is how the board learns the Mac's IP (to dial the lyrics daemon) and the current server time (the waiting screen's clock); the numbers feed the dashboard |
| `/usage.json` | GET | Full device state as JSON, including `lyr` (0 waiting / 1 dialing / 2 streaming), `wifi`, `ssid`, `ip`, `rssi` |
| `/nowplaying` | GET/POST | Daemon pushes song title/artist/position/lyrics — also IP-learning; shown on the dashboard, not on the panel |
| `/brightness` | GET/POST | TFT backlight PWM, persisted across reboots |
| `/wifi-reset` | GET/POST | Forget the saved WiFi, reboot into the setup hotspot |
| `/factory-reset` | GET/POST | That, plus stored settings back to defaults |
| `/restart` | GET/POST | Reboot |
| `/update` | GET/POST | OTA firmware upload |

## Editing the dashboard

- The dashboard HTML lives in the `INDEX_HTML` raw string inside the sketch.
  After editing it run `python3 esp8266/tools/gen_index_gz.py` to refresh
  `index_html_gz.h` (the sketch serves the gzipped copy; a 15 KB uncompressed
  send truncates on lossy WiFi, and the generator enforces a 5 KB gzip cap).
- Lyrics text (including Thai) is rendered on the Mac by the lyrics daemon, not
  on-device — there are no bundled fonts to regenerate here.

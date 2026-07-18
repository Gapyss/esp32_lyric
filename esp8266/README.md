# Clawdmeter — ESP8266 desk cube

## What is this?

A tiny desk display that shows your **Claude usage limits** (session + weekly
window) at a glance, plus a set of small companion screens — a clock, a
pomodoro timer, hydration reminders, now-playing music with synced lyrics, a
pettable pet, a falling-sand toy, a swarm, and the daily xkcd / NASA APOD
picture. Everything is drawn in the **Tend** look shared with the ESP32
firmware in this repo: warm paper, ink, one ember accent.

It runs on the GeekMagic **HelloCubic Lite** / **SmallTV-Ultra** (an ESP8266
with an ST7789 240×240 color TFT), ported from
[Gapyss/clawdmeter-esp8266](https://github.com/Gapyss/clawdmeter-esp8266).
The device drives the LCD **and** serves a web dashboard at the same URL, so
you get the meter on the desk and in the browser.

```
Mac daemon ──poll 60s──> api.anthropic.com   (reads usage headers)
   └── HTTP POST /usage + /nowplaying + /daily ──> ESP8266 ──serves──> dashboard

lyrics daemon (lyrics_display_daemon.py, optional)
   └── WebSocket :8766 ──240x240 1-bit frames──> ESP8266 MUSIC screen
```

Your Anthropic OAuth token never leaves the Mac. The device only receives
usage percentages, reset times, Mac metrics, now-playing/lyric lines, and
daily-image metadata.

### The screens

Switched from the dashboard (the device has no buttons — the ESP32's button
gestures became dashboard controls):

| Screen | What it shows / does |
|---|---|
| `clock` | Time from the daemon push (no RTC/NTP needed once pushed) |
| `music` | Now-playing title/artist, progress, synced lyrics. When the Mac's `lyrics_display_daemon.py` is running, the screen upgrades itself to a streamed 240×240 frame from the Mac (Core Text Thai shaping + syllable karaoke, same pipeline as the ESP32); otherwise it renders on-device (Thai+Latin fonts) |
| `pomodoro` | Focus timer — start/pause/reset from the dashboard |
| `water` | Hydration reminders — log drinks, snooze, configurable window |
| `stats` | Claude usage + Mac metrics trend |
| `pet` | A pettable pet; pets counted per-day and all-time |
| `sand` | Falling-sand toy — pour and clear |
| `swarm` | A swarm that scatters on demand or roams |
| `comic` / `apod` | Daily xkcd / NASA APOD, dithered 1-bit via the wsrv.nl proxy |

## What you need

- A GeekMagic HelloCubic Lite / SmallTV-Ultra (or any ESP8266 + ST7789
  240×240 board — pins are set at the top of the sketch).
- A USB data cable for the **first** flash (updates after that are OTA).
- Arduino IDE or `arduino-cli` on your computer.
- A Mac on the same WiFi to run `daemon/claudemeter_daemon.py` (it reads the
  usage headers and pushes them to the device).

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

On first boot the device has no saved network, so it opens its own hotspot:

1. The LCD shows a setup hint and a hotspot appears: **Clawdmeter-setup**.
2. Join it from your phone — a captive setup portal pops up automatically.
3. Pick your WiFi, enter the password. The device saves it and reboots onto
   your network.

The credentials persist across reboots and OTA updates.

**Changing WiFi later / recovery:** if the device can't reach its saved
network (new router, new password, moved house), it reopens the
**Clawdmeter-setup** hotspot by itself — rejoin and pick the new network. No
reflashing. The portal times out after 3 minutes and retries, so a temporary
router outage just means it reconnects when the network returns.

**Full reset:** the dashboard's device panel has a **reset settings** button
(`/factory-reset`). It wipes the saved WiFi and stored settings (brightness,
hydration schedule) and reboots straight back into the Clawdmeter-setup
hotspot.

### 3. Open the dashboard

Visit `http://clawdmeter.local/` (or the IP printed on the Serial Monitor at
115200 baud). You'll see the usage meters showing *waiting for daemon* until
step 4.

- The **screen** row switches the physical LCD between the screens above.
- The panel under it shows the live screen's controls — start the pomodoro,
  log a drink, pet the pet, pour sand, scatter the swarm, refresh the comic.
- The **backlight** slider tunes the TFT backlight; the value is saved on the
  device and survives reboot.

### 4. Start the Mac daemon

The daemon defaults to the ESP32 render wrapper with usage polling off, so
point it at the board and turn polling on:

```sh
CLAWDMETER_DEVICE_URL=http://clawdmeter.local \
CLAWDMETER_USAGE_SOURCE=api \
python3 daemon/claudemeter_daemon.py
```

- `CLAWDMETER_USAGE_SOURCE=api` reads Anthropic response headers (closest
  match to Claude's real server-side limits; macOS may pop a Keychain prompt
  for the `Claude Code-credentials` item — click **Always Allow**). Use
  `local` to scan Claude Code's local JSONL transcripts instead (set
  `CLAWDMETER_SESSION_TOKEN_LIMIT` / `CLAWDMETER_WEEKLY_TOKEN_LIMIT` to taste).
- Now-playing/lyrics pushes to `/nowplaying` work with no extra configuration.
- **Streamed lyrics (optional):** if the lyrics daemon
  (`daemon/lyrics_display_daemon.py` + the browser extension) is running on
  the same Mac, the MUSIC screen automatically upgrades from on-device text
  to Mac-rendered 240×240 frames — real Thai shaping and syllable-karaoke
  highlighting over WebSocket (`:8766/board?w=240&h=240`). Zero configuration:
  the device learns the Mac's IP from the `/usage`//`/nowplaying` pushes and
  dials back. If the stream drops (Mac asleep, daemon stopped), the screen
  falls back to the on-device renderer within seconds. Note the ESP8266
  client does not send a board token, so leave `G4PYS_LYRICS_BOARD_TOKEN`
  unset (its default).
- The daemon also resolves the daily xkcd/APOD metadata and pushes it to
  `/daily` every 6 h (the ESP8266 can't afford the TLS heap to call those
  HTTPS APIs itself). It turns on automatically with usage pushing; set
  `CLAWDMETER_NASA_API_KEY` (free at https://api.nasa.gov) to avoid the shared
  `DEMO_KEY` rate limit, or `CLAWDMETER_DAILY_IMAGES=off` to disable.

To run it under launchd instead, edit
`launchd/company.g4pys.claudemeter-daemon.plist`: set `CLAWDMETER_DEVICE_URL`
to `http://clawdmeter.local` and `CLAWDMETER_USAGE_SOURCE` to `api`, then:

```sh
launchctl unload ~/Library/LaunchAgents/company.g4pys.claudemeter-daemon.plist
launchctl load  ~/Library/LaunchAgents/company.g4pys.claudemeter-daemon.plist
```

### 5. Updating later (OTA, no USB)

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
│   ├── clawdmeter_esp8266.ino   # the sketch: HTTP API + INDEX_HTML dashboard
│   ├── tend.h                   # shared Tend UI kit + screen interfaces
│   ├── clock_screen.cpp         # … one .cpp per screen (ported from firmware/main)
│   ├── pomodoro_screen.cpp, water_screen.cpp, stats_screen.cpp,
│   ├── pet_screen.cpp, sand_screen.cpp, swarm_screen.cpp, daily_screen.cpp
│   ├── lyrics_stream.cpp        # MUSIC frame stream: WS client for the lyrics daemon
│   ├── tjpgd.c/.h, tjpgdcnf.h   # vendored TJpgDec (streams comic/APOD JPEGs)
│   ├── index_html_gz.h          # generated — gzipped dashboard served at /
│   └── thai_font.h              # generated — Thai/Latin GFX fonts for the MUSIC screen
└── tools/
    ├── gen_index_gz.py          # regenerate index_html_gz.h after editing INDEX_HTML
    └── gen_thai_font.sh         # regenerate thai_font.h (needs Adafruit fontconvert)
```

## Device HTTP API

| Endpoint | Methods | Purpose |
|---|---|---|
| `/` | GET | Web dashboard (gzipped, from PROGMEM) |
| `/usage` | POST | Daemon pushes usage % + Mac metrics + server time |
| `/usage.json` | GET | Full device state as JSON (dashboard polls this) |
| `/nowplaying` | GET/POST | Daemon pushes song title/artist/position/lyrics |
| `/mode` | GET/POST | `?screen=` clock/music/pomodoro/water/stats/pet/sand/swarm/comic/apod |
| `/pomodoro` | GET/POST | `?action=toggle` (default) or `reset` |
| `/hydrate/log`, `/hydrate/now` | GET/POST | Log a drink / fire the reminder now |
| `/hydrate/snooze` | GET/POST | `?min=10` snooze the reminder |
| `/hydrate/config` | GET/POST | `?interval=45&start=09:00&end=18:00` (any subset) |
| `/pet` | GET/POST | Pet the pet |
| `/sand` | GET/POST | `?action=pour` (default) or `clear` |
| `/swarm` | GET/POST | `?action=scatter` (default) or `roam` |
| `/daily` | GET/POST | Daemon pushes xkcd/APOD image URL + title metadata |
| `/refresh` | GET/POST | `?screen=comic|apod` — refetch the daily image |
| `/brightness` | GET/POST | TFT backlight PWM, persisted across reboots |
| `/restart`, `/factory-reset` | GET/POST | Maintenance |
| `/update` | GET/POST | OTA firmware upload |

## Editing the dashboard or fonts

- The dashboard HTML lives in the `INDEX_HTML` raw string inside the sketch.
  After editing it run `python3 esp8266/tools/gen_index_gz.py` to refresh
  `index_html_gz.h` (the sketch serves the gzipped copy; a 15 KB uncompressed
  send truncates on lossy WiFi, and the generator enforces a 5 KB gzip cap).
- Thai fonts for the MUSIC screen are generated by
  `esp8266/tools/gen_thai_font.sh` (needs Adafruit fontconvert; see the script
  header).

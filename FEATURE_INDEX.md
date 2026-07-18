# Feature Index

This is a map of the project features and the files that implement or document them.

Status legend:

- Implemented: code exists in the repo.
- Service config: launchd or packaging support exists.
- Draft/design: documented, but may not be the active runtime path.
- Legacy/alternate: older or parallel flow still present in the repo.

## Product Goal

The project turns a Waveshare ESP32-S3-RLCD-4.2 board into a multi-mode 400x300 monochrome ambient display. It provides synced lyrics for music playing on a Mac alongside hydration, productivity, clock, simulation, daily comic, and astronomy screens. Audio playback stays on the Mac; the board uses its speaker only for local notification chimes.

Primary references:

- Board and pin spec: `spec.md`
- WebSocket lyrics-display architecture: `lyrics-display-design.md`
- Firmware handoff and HTTP wrapper lineage: `firmware/PLAN.md`
- Thai text rendering rationale: `thai_font_approach.md`

## Current WebSocket Lyrics Display Path

### YouTube Music Browser Extension

Status: Implemented

Files:

- `browser_extension/manifest.json`
- `browser_extension/content_script.js`
- `browser_extension/README.md`
- `tools/package_browser_extension.py`
- `dist/g4pys-lyrics-display-bridge-0.1.0.zip`

Features:

- Runs as a Manifest V3 content script on `https://music.youtube.com/*`.
- Reads the active `<video>` element for position, duration, pause state, and playback rate.
- Reads `navigator.mediaSession.metadata` plus DOM fallbacks for title, artist, and album.
- Extracts YouTube Music `videoId` from the current URL or player links.
- Opens `ws://127.0.0.1:8765/extension` to the local lyrics daemon.
- Sends `now-playing`, `tick`, and `event` messages.
- Sends play, pause, seek, ended, and rate-change events from the video element.
- Reconnects to the daemon automatically.
- Package script builds a deterministic extension zip and can optionally delegate Firefox unlisted signing to `web-ext`.

### Local Lyrics Display Daemon

Status: Implemented

Files:

- `daemon/lyrics_display_daemon.py`
- `tests/test_lyrics_display_daemon.py`
- `launchd/company.g4pys.lyrics-display-daemon.plist`

Features:

- Hosts a WebSocket server for browser extension clients on `/extension`.
- Hosts a WebSocket server for ESP32 board clients on `/board`.
- Optionally protects board WebSocket access with `G4PYS_LYRICS_BOARD_TOKEN`.
- Tracks current song metadata and playback clock.
- Interpolates playback position between extension ticks.
- Handles play, pause, seek, rate-change, track-change, and ended events.
- Resolves lyrics from SQLite before network access.
- Gives `manual` SQLite rows priority over `lrclib` cache rows.
- Fetches lyrics from lrclib with `/api/get`, then `/api/search` fallback.
- Prefers synced lyrics, falls back to plain lyrics or instrumental.
- Stores positive and negative lrclib results in SQLite.
- Expires negative cache rows after the configured TTL.
- Imports manual `.lrc` files into SQLite with the `import-lrc` CLI.
- Provides cache `stats` and `clear` CLI commands.
- Parses standard LRC timestamps and inline enhanced-LRC syllable markers.
- Renders 400x300 1-bit frames on the Mac.
- Also renders a compact 240x240 profile for ESP8266 boards (`/board?w=240&h=240`).
- Keeps per-profile frame caches; scheduled swaps render every active profile.
- Uses Core Text rendering when PyObjC is available.
- Uses a geometry-only fallback renderer when Core Text is unavailable.
- Generates `LYR1` binary frame envelopes for the ESP32 board.
- Sends full-frame updates and dirty-rectangle updates.
- Schedules future lyric-line frames with relative `swapInMs`.
- Re-sends the current frame when a board sends `ready`.
- Advertises `_lyrics._tcp` with strict `proto=2`, HMAC auth, and daemon UUID TXT metadata.
- Persists the Mac daemon identity in a mode-0600 JSON file and provides a `pairing-token` CLI.
- Uses nonce/HMAC mutual authentication, directional SEC2 keys, replay-protected records, and heartbeat timeouts.
- Gives every board an independent bounded outbound queue so a slow board is disconnected in isolation.
- launchd job starts the daemon at login and keeps it alive.

Default SQLite location:

- `G4PYS_LYRICS_DB`, or `~/.g4pys/lyrics-display.sqlite3`

### WebSocket Frame Protocol

Status: Implemented

Files:

- `daemon/lyrics_display_daemon.py`
- `firmware/main/board_client.cpp`
- `firmware/main/music_screen.cpp`
- `firmware/main/display_config.h`
- `lyrics-display-design.md`

Features:

- Binary frame envelope magic: `LYR1`.
- Protocol version: `1`.
- Supports full frames and byte-aligned dirty rectangles.
- Supports immediate and scheduled updates.
- Carries display dimensions, rectangle coordinates, row bytes, `swapInMs`, and payload byte count.
- Full frame size is 15000 bytes for 400x300 at 1 bit per pixel.
- Firmware also accepts an older JSON-header plus binary-payload compatibility format.

### ESP32 WebSocket Board Client

Status: Implemented

Files:

- `firmware/main/board_client.cpp`
- `firmware/main/board_client.h`
- `firmware/main/music_screen.cpp`
- `firmware/main/display_config.h`
- `firmware/main/network_manager.cpp`
- `firmware/main/provisioning_portal.cpp`

Features:

- Loads the pairing token and preferred daemon UUID from NVS, never a daemon IP.
- Discovers `_lyrics._tcp` on every connection attempt and strictly filters proto=2/auth/UUID TXT.
- Prefers the remembered UUID and same-subnet, interface-correct Bonjour IPv4 addresses.
- Performs nonce/HMAC mutual authentication and exchanges SEC2-protected hello/ready records.
- Receives `LYR1` full-frame and rect-frame envelopes.
- Applies immediate frames immediately.
- Stages scheduled frames and swaps them after `swapInMs`.
- Exposes `PAIRING_REQUIRED` without preventing Wi-Fi and accepts pairing through setup or `/pairing`.
- Auto-reconnects with jittered exponential discovery retry capped at 30 seconds.
- Enforces a 20-second ping / 10-second pong heartbeat and re-discovers after every reconnect.
- Keeps proto=1 behind the explicit `LYRICS_ALLOW_LEGACY_PROTO1` migration flag without downgrade.
- Clears daemon-supplied full frames after disconnect so idle UI can return.

## Firmware Runtime

### ESP-IDF App Bootstrap

Status: Implemented

Files:

- `firmware/main/main.cpp`
- `firmware/main/CMakeLists.txt`
- `firmware/CMakeLists.txt`
- `firmware/main/idf_component.yml`
- `firmware/sdkconfig.defaults`
- `firmware/partitions.csv`
- `firmware/dependencies.lock`

Features:

- ESP-IDF firmware for ESP32-S3.
- Initializes NVS, display, I2C peripherals, render task, WiFi, mDNS, HTTP API, and WebSocket board client.
- Uses compile-time WiFi credentials from `firmware/main/wifi_secrets.h`.
- Publishes device hostname `g4pys-company.local`.
- Starts an HTTP server on port 80.
- Starts a render task pinned to core 1.

### Modes and Physical Controls

Status: Implemented

Files:

- `firmware/main/app_mode.cpp`
- `firmware/main/app_mode.h`
- `firmware/main/app_config.h`
- `firmware/main/main.cpp`

The default screen is `water`. A short press of the BOOT button on GPIO0 cycles through:

`music` -> `water` -> `stats` -> `pomodoro` -> `clock` -> `pet` -> `sand` -> `swarm` -> `comic` -> `apod` -> `music`

The secondary button on GPIO18 performs an action based on the active screen:

| Mode | Short press | Hold for 0.8 seconds |
|---|---|---|
| Water | Log a drink | No action |
| Pomodoro | Start or pause | Reset timer |
| Pet | Pet the creature | No action |
| Sand | Pour sand | Clear the field |
| Swarm | Scatter the fireflies | Toggle time/roam behavior |
| Comic | Refresh XKCD | No action |
| APOD | Refresh NASA APOD | No action |

Modes can also be selected over HTTP with `GET /mode?set=<mode>`. Calling `GET /mode` without `set` returns the current mode name.

### ST7305 Display Driver Integration

Status: Implemented

Files:

- `firmware/main/main.cpp`
- `firmware/components/u8g2_st7305/`
- `firmware/components/u8g2/`
- `firmware/main/display_config.h`

Features:

- Drives the Waveshare 4.2 inch 400x300 ST7305 reflective LCD.
- Uses `u8g2_st7305` with `SPI3_HOST`.
- Uses pins documented in `spec.md`: MOSI 12, SCK 11, DC 5, CS 40, RST 41.
- Uses `U8G2_R1` rotation for a 400x300 landscape canvas.
- Uses full-frame buffer mode.
- Renders through u8g2.

### Music Screen Renderer

Status: Implemented

Files:

- `firmware/main/music_screen.cpp`
- `firmware/main/music_screen.h`

Features:

- Maintains current now-playing state behind a FreeRTOS mutex.
- Supports text-based now-playing updates.
- Supports bitmap-slot now-playing updates.
- Supports full-frame framebuffer updates from the WebSocket daemon.
- Supports dirty-rectangle framebuffer patches.
- Handles scheduled full-frame and rect-frame swaps.
- Locally interpolates elapsed playback time from position and monotonic time.
- Promotes lyric lines locally when `lt` or `lt2` boundaries are reached.
- Renders title, artist, progress bar, current lyric, next lyric, and footer.
- Renders idle screen when no title is active.
- Draws local clock and temperature/humidity on idle screen when available.
- Supports marquee scrolling for wide title, artist, and lyric content.
- Supports slide transition animation between lyric lines.
- Supports rendered bitmap slots for shaped text.
- Falls back to u8g2 text rendering when bitmap slots are not present.

### Idle Peripherals

Status: Implemented

Files:

- `firmware/main/board_peripherals.cpp`
- `firmware/main/board_peripherals.h`
- `spec.md`

Features:

- Initializes I2C on GPIO13 and GPIO14.
- Reads PCF85063 RTC time.
- Reads SHTC3 temperature and humidity.
- Validates SHTC3 CRC.
- Supplies idle screen metrics to `music_screen`.

### Firefly Clock Screen

Status: Implemented

Files:

- `firmware/main/swarm_screen.cpp`
- `firmware/main/swarm_screen.h`

Features:

- Simulates 300 independently moving fireflies using fixed-point positions and velocities.
- Attracts the swarm into large seven-segment clock digits when RTC time is available.
- Uses gentler movement between 22:00 and 07:00.
- Short action-button presses scatter the swarm for 1.6 seconds.
- Long action-button presses toggle between clock formation and free-roaming behavior.

### Daily XKCD and NASA APOD Screens

Status: Implemented

Files:

- `firmware/main/comic_screen.cpp`
- `firmware/main/comic_screen.h`
- `firmware/components/pngle/`
- `firmware/main/idf_component.yml`

Features:

- Fetches the latest XKCD metadata and PNG from `https://xkcd.com/info.0.json`.
- Fetches NASA Astronomy Picture of the Day metadata from the official APOD API.
- Fetches both sources at startup and automatically refreshes them every six hours.
- Allows an immediate refresh with the secondary button while the corresponding screen is active.
- Keeps the previous image visible with an `UPDATING` indicator while a refresh is running.
- Downloads and decodes images in a single background task so PNG and JPEG work do not compete for TLS and decode memory.
- Places downloaded images and decode buffers in PSRAM.
- Scales XKCD to the 238-pixel image area while retaining up to 720 pixels of width.
- Slowly pans wide XKCD strips horizontally so text is larger and remains readable on the small screen.
- Decodes APOD JPEG images with `espressif/esp_jpeg`, normalizes contrast, and applies ordered monochrome dithering.
- Uses `thumbnail_url` when the APOD entry is a video.
- Displays source title, XKCD number, or APOD publication date around the image.
- Shows an on-screen error state when WiFi, HTTP, parsing, memory allocation, or decoding fails.

The firmware uses NASA's `DEMO_KEY` by default. For a private key, copy
`firmware/main/api_credentials.example.h` to `api_credentials.h` and set the
`NASA_API_KEY` string macro. The local credentials file is ignored by Git.

The APOD screen changes after NASA publishes a new entry and the next refresh succeeds. It does not switch exactly at local midnight; with automatic refresh enabled, a newly published image may take up to six hours to appear. Restarting the board or pressing the secondary button requests it immediately.

### HTTP API on the ESP32

Status: Implemented

Files:

- `firmware/main/http_api.cpp`
- `firmware/main/http_api.h`
- `firmware/main/music_screen.cpp`

Features:

- `GET /nowplaying` accepts URL query now-playing fields.
- `POST /nowplaying` accepts query numeric fields plus binary bitmap-slot body.
- Percent-decodes UTF-8 query values.
- `GET /usage.json` returns current display state for debugging.
- `GET /mode` returns the active firmware mode.
- `GET /mode?set=<mode>` selects `music`, `water`, `stats`, `pomodoro`, `clock`, `pet`, `sand`, `swarm`, `comic`, or `apod`.
- `GET /diag/display` can show orientation, polarity, or timing diagnostic frames.
- `GET /diag/display?pattern=clear` clears full-frame override mode.

## Alternate HTTP Render-Wrapper Path

This path is still implemented and configured, but it is separate from the newer browser-extension to WebSocket-daemon to board-client path.

### Clawdmeter Daemon

Status: Legacy/alternate implemented path

Files:

- `daemon/claudemeter_daemon.py`
- `launchd/company.g4pys.claudemeter-daemon.plist`
- `render.md`
- `thai_font_approach.md`
- `firmware/PLAN.md`

Features:

- Polls YouTube Music now-playing state from macOS using AppleScript.
- Can enrich track metadata through `yt-dlp`.
- Resolves lyrics through lrclib HTTP API.
- Optionally uses a downloaded lrclib SQLite dump through `CLAWDMETER_LRCLIB_DB`.
- Maintains persistent on-demand lyric cache in `CLAWDMETER_LYRIC_CACHE`, defaulting to `~/.clawdmeter/lyrics.sqlite3`.
- Parses synced and plain lyrics.
- Computes current, next, and third lyric lookahead lines.
- Pushes `/nowplaying` updates to `CLAWDMETER_DEVICE_URL`.
- Can poll Claude usage from API headers, server API, or local usage files, though the included launchd config disables usage with `CLAWDMETER_USAGE_SOURCE=off`.
- launchd job points this daemon at the local render wrapper on `http://127.0.0.1:8123`.

### Core Text Render Wrapper

Status: Legacy/alternate implemented path

Files:

- `tools/g4pys_render_wrapper.py`
- `tools/requirements-render-wrapper.txt`
- `launchd/company.g4pys.render-wrapper.plist`
- `thai_font_approach.md`

Features:

- Runs a local HTTP server, default `127.0.0.1:8123`.
- Accepts `/nowplaying` GET or POST from `claudemeter_daemon.py`.
- Renders title, artist, lyric, lyric2, and lyric3 text slots with Core Text.
- Produces 1-bit bitmap-slot payloads for correct Thai and complex text shaping.
- Packs each slot as little-endian `width`, `height`, `data_len`, then 1-bpp data.
- Forwards numeric timing fields unchanged to the ESP32.
- Sends bitmap body to the device `/nowplaying` endpoint.
- launchd job starts the wrapper with configured font and device URL.

## Browser Extension Packaging

Status: Implemented

Files:

- `tools/package_browser_extension.py`
- `browser_extension/README.md`
- `dist/g4pys-lyrics-display-bridge-0.1.0.zip`

Features:

- Validates required Manifest V3 keys.
- Builds deterministic zip output in `dist/`.
- Includes `manifest.json`, `content_script.js`, and `README.md`.
- Optional Firefox unlisted signing through `web-ext`.

## Test Coverage

Status: Implemented

Files:

- `tests/test_lyrics_display_daemon.py`

Covered areas:

- SQLite store thread-safety and worker-thread usage.
- Cache read and cache write behavior.
- Negative cache rows.
- Manual rows surviving lrclib cache clears.
- lrclib `get` to `search` fallback behavior.
- Synced versus plain lyric candidate preference.
- LRC and enhanced-LRC parsing.
- WebSocket frame parsing and server frame writing.
- Board handshake authorization.
- Frame envelope creation and scheduled frame behavior.
- Dirty-rectangle frame generation.
- Daemon state transitions around track updates and lyric resolution.

## Hardware and Board Support

Status: Documented and implemented in firmware

Files:

- `spec.md`
- `firmware/main/main.cpp`
- `firmware/main/board_peripherals.cpp`
- `firmware/components/u8g2_st7305/`

Features:

- Targets Waveshare ESP32-S3-RLCD-4.2.
- Uses ESP32-S3-WROOM-1 N16R8 with 16 MB flash and 8 MB PSRAM.
- Uses native USB-Serial/JTAG.
- Uses ST7305 400x300 reflective monochrome LCD.
- Documents display, I2C, microSD, ADC, and audio-related pin maps.
- Firmware currently uses display, WiFi, mDNS, HTTP, RTC, and SHTC3 paths.
- Audio hardware is documented but intentionally out of scope.
- microSD and battery ADC are documented but not implemented in the current firmware feature set.

## Service Configuration

Status: Service config

Files:

- `launchd/company.g4pys.lyrics-display-daemon.plist`
- `launchd/company.g4pys.claudemeter-daemon.plist`
- `launchd/company.g4pys.render-wrapper.plist`

Features:

- `company.g4pys.lyrics-display-daemon` starts `daemon/lyrics_display_daemon.py serve`.
- `company.g4pys.lyrics-display-daemon` sets `G4PYS_LYRICS_DB=/Users/g4pys/.g4pys/lyrics-display.sqlite3`.
- `company.g4pys.claudemeter-daemon` starts `daemon/claudemeter_daemon.py`.
- `company.g4pys.claudemeter-daemon` forwards to `http://127.0.0.1:8123`.
- `company.g4pys.render-wrapper` starts `tools/g4pys_render_wrapper.py`.
- `company.g4pys.render-wrapper` forwards to `http://g4pys-company.local`.
- All three jobs use `RunAtLoad` and `KeepAlive`.

## Design Notes and Drafts

Status: Draft/design

Files:

- `lyrics-display-design.md`
- `render.md`
- `thai_font_approach.md`
- `firmware/PLAN.md`
- `firmware/HYDRATION_PLAN.md`
- `firmware/CLAUDE_REVIEW.md`
- `docs/lyrics-display-agent-review.md`

Topics:

- Browser extension security rationale versus third-party YouTube Music desktop clients.
- Why ESP32-S3 cannot act as a Bluetooth Classic A2DP speaker.
- Why the daemon owns scheduling and SQLite instead of the browser or board.
- Frame-envelope protocol and dirty-rect design.
- SQLite schema and cache policy.
- Thai rendering tradeoffs and Mac-side shaping.
- Firmware implementation order and validation notes.
- Review and handoff notes from previous implementation passes.

## Explicitly Out of Scope or Not Yet Implemented

Items documented but not active features in the current implementation:

- Bluetooth speaker or A2DP sink mode.
- On-device audio playback through ES8311/ES7210.
- Captive portal or runtime WiFi manager.
- OTA update UI.
- Browser dashboard UI.
- Battery/sleep behavior.
- microSD features.
- Battery voltage ADC display.
- Full production browser-store release workflow beyond packaging/signing helpers.

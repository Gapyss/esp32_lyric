# Lyrics Display Implementation Review Handoff

This repo now has the browser-extension, daemon, firmware board-client, idle-screen,
maintenance CLI, and packaging slices for `lyrics-display-design.md`.

## What changed

- Added `browser_extension/manifest.json` and `browser_extension/content_script.js`.
  - Runs on `https://music.youtube.com/*`.
  - Reads `navigator.mediaSession.metadata`, the page URL/link `v` parameter, fallback YouTube Music DOM selectors, and the active `<video>` element.
  - Uses a `MutationObserver` plus polling so track metadata updates survive common YouTube Music DOM timing changes.
  - Streams `now-playing`, `tick`, and `event` JSON messages to `ws://127.0.0.1:8765/extension`.
- Added `browser_extension/README.md` and `tools/package_browser_extension.py`.
  - Builds a deterministic upload zip in `dist/`.
  - Validates the MV3 manifest before packaging.
  - Optionally delegates Firefox unlisted signing to `web-ext sign` when `WEB_EXT_API_KEY` / `WEB_EXT_API_SECRET` are set.
- Added `daemon/lyrics_display_daemon.py`.
  - Uses only the Python standard library for WebSocket server handling.
  - Accepts extension WebSocket messages on localhost.
  - Maintains a playback clock with local interpolation from the last tick.
  - Creates the `lyrics` SQLite schema from the design doc.
  - Resolves lyrics in this order: manual SQLite row, lrclib cache row, lrclib network fetch.
  - Serializes SQLite access so cache reads and lrclib resolution can run in worker threads without blocking the event loop or breaking cache reads/writes.
  - Distinguishes unresolved lyrics from a resolved negative cache row, so lyric-less tracks fall back to the now-playing layout instead of staying on "Resolving lyrics...".
  - Renders a 400x300 1-bpp framebuffer using macOS Core Text when PyObjC is installed.
  - Exposes a board-facing WebSocket on port `8766`; sends each framebuffer update as one self-describing binary WebSocket message.
  - Advertises `_lyrics._tcp` through a native stdlib mDNS responder, with macOS `dns-sd` fallback if native multicast setup fails; use `--no-mdns` to disable it.
  - Supports optional board WebSocket token auth through `G4PYS_LYRICS_BOARD_TOKEN` / `--board-token`.
  - Sends dirty-rectangle binary envelopes for immediate frame changes after each board has received an initial full frame.
  - For synced lyrics, sends the current frame immediately and pre-sends the next lyric-line frame with `swapInMs`; a daemon-side scheduler advances the queue after each line boundary.
  - Parses enhanced LRC inline syllable timestamps like `<00:12.34>` and renders a simple active-line highlight progress marker.
  - Keeps synced lyrics blank until playback reaches the first line timestamp, so intro text is not shown prematurely.
  - Adds maintenance commands:
    - `import-lrc` writes `.lrc` files as `source='manual'`.
    - `cache stats` reports row counts.
    - `cache clear --source lrclib` deletes only lrclib rows.
- Added `launchd/company.g4pys.lyrics-display-daemon.plist`.
  - Runs `daemon/lyrics_display_daemon.py serve` as a per-user LaunchAgent.
  - Uses `RunAtLoad` and `KeepAlive`, with logs under `~/Library/Logs`.
- Added `firmware/main/board_client.cpp` and `firmware/main/board_client.h`.
  - Discovers the daemon through `_lyrics._tcp` mDNS, with optional `LYRICS_DAEMON_HOST` / `LYRICS_DAEMON_PORT` fallback in `wifi_secrets.h`.
  - Performs a minimal plaintext WebSocket client handshake to `/board`, optionally adding `LYRICS_DAEMON_TOKEN` as a query token.
  - Sends `ready`, responds to ping with pong, validates daemon frame envelopes, and accepts only `400x300` `1bpp-lsb-rowmajor` payloads.
  - Also keeps backward-compatible support for the older JSON-header plus binary-payload frame format.
  - Stores full-frame payloads in `music_screen` framebuffer mode and supports `swapInMs` by staging a pending frame.
  - Clears framebuffer mode on daemon disconnect so the display falls back to the board-local/non-playing screen.
- Added `firmware/main/board_peripherals.cpp` and `firmware/main/board_peripherals.h`.
  - Initializes the shared GPIO13/GPIO14 I2C bus.
  - Reads PCF85063 hour/minute and SHTC3 temperature/humidity for the idle screen.
- Updated `firmware/main/music_screen.cpp`.
  - Replaces the placeholder non-playing screen with the RTC + temperature/humidity idle layout.
  - Adds framebuffer dirty-rectangle patching support.
- Updated `firmware/main/http_api.cpp`.
  - Adds `/diag/display?pattern=orientation|polarity|timing|clear` for repeatable real-board display validation.

The browser extension opens `ws://127.0.0.1:8765/extension` directly from the content script. That WebSocket is initiated by the isolated content script and is not governed by YouTube Music's page CSP.

The manifest currently keeps `http://127.0.0.1/*` and `http://localhost/*` in `host_permissions`. Treat those as unverified compatibility entries, not a proven requirement. To settle this, load `browser_extension/` unpacked in Chrome, check `chrome://extensions` for permission warnings, remove the two localhost entries, reload the extension, and confirm the daemon still logs the extension connection from YouTube Music.

## Current protocol shape

Extension to daemon:

```json
{"type":"now-playing","payload":{"videoId":"...","title":"...","artist":"...","album":"...","durationSec":123}}
{"type":"tick","payload":{"positionSec":42.3,"durationSec":123,"paused":false,"playbackRate":1}}
{"type":"event","payload":{"type":"play","positionSec":42.3,"paused":false,"playbackRate":1}}
```

Daemon to board binary envelope:

```text
magic[4] = "LYR1"
version u8 = 1
kind u8 = 1 full-now, 2 full-scheduled, 3 rect-now, 4 rect-scheduled
width u16, height u16
x u16, y u16, rectWidth u16, rectHeight u16, rowBytes u16
swapInMs u32
payloadBytes u32
payload bytes = full framebuffer or byte-aligned rect
```

The envelope and framebuffer/rect payload are carried in one binary WebSocket message, so there is no header/payload pairing state and no lock-across-messages requirement.

## Review focus

- The daemon-to-board two-message frame protocol has been replaced by a single self-describing binary envelope. Firmware still accepts the old form as a compatibility fallback.
- Dirty-rectangle updates are retained for immediate frame changes after an initial full frame. They save bandwidth but add per-board framebuffer-base state; full-frame-only updates would still be simpler if LAN bandwidth proves irrelevant.
- Predictive pre-send scheduling is load-bearing for synced lyrics. It lets line changes happen at the timestamp boundary instead of waiting for the next browser-extension tick.
- Check the extension’s metadata strategy against live YouTube Music. It now uses MediaSession, URL/link video IDs, DOM selector fallbacks, and mutation observation, but this remains inherently brittle against site changes.
- WebSocket handling is still intentionally small and server-only, but now rejects unmasked client frames, reassembles continuation frames, handles ping/pong without recursion, replies to close frames, and serializes writes per connection.
- Check Core Text coordinate math and bit polarity against the ST7305/u8g2 path on real hardware. The `/diag/display` endpoint exists for orientation/polarity/timing validation, but physical validation has not been performed by Codex.
- lrclib cache semantics: negative rows are represented as `lrc = NULL` with `source='lrclib'`, expire after 14 days, and render as the normal now-playing view while they are valid. `/api/get` accepts synced, plain, or instrumental lyrics; `/api/search` prefers synced candidates and falls back to the best duration-matched plain/instrumental candidate.
- Verify token-auth rollout with the board config. If `G4PYS_LYRICS_BOARD_TOKEN` is set on the daemon, firmware must define matching `LYRICS_DAEMON_TOKEN`.

## Known gaps

- ESP32 firmware still keeps the older HTTP `/nowplaying` bitmap-slot path for debug/backward compatibility.
- Predictive scheduling is still line-boundary based. Enhanced LRC syllable markers render highlight progress, but the scheduler does not prequeue every syllable boundary. If a browser tick lands within `MIN_SCHEDULE_SWAP_MS` of a line boundary, that one flip may wait for the next tick instead of being staged.
- Board WebSocket auth is optional and off by default for bring-up. Set `G4PYS_LYRICS_BOARD_TOKEN` and matching firmware `LYRICS_DAEMON_TOKEN` to require it.
- Chrome Web Store signing is still handled by Chrome Web Store upload. The local workflow produces an upload zip; Firefox signing is delegated to `web-ext`.
- Real hardware validation still needs to be run after flashing. Use `/diag/display` patterns plus a live daemon/extension session to confirm polarity, orientation, refresh timing, RTC, and SHTC3 readings.

## Local verification

Recommended checks for the next agent:

```sh
python3 -c "import py_compile; py_compile.compile('daemon/lyrics_display_daemon.py', cfile='/tmp/lyrics_display_daemon.pyc', doraise=True)"
python3 -m unittest tests.test_lyrics_display_daemon
python3 tools/package_browser_extension.py
cd firmware && idf.py build
python3 daemon/lyrics_display_daemon.py serve --db /tmp/lyrics-display.sqlite3
python3 daemon/lyrics_display_daemon.py cache --db /tmp/lyrics-display.sqlite3 stats
```

In the Codex managed sandbox, `idf.py build` may need escalated execution if ESP-IDF's component manager uses process inspection that macOS denies in the sandbox. Binding the daemon's localhost ports may also require escalated execution. In the current base interpreter, PyObjC/CoreText is not installed, so the daemon starts with its geometry-only fallback renderer until the existing PyObjC requirements are installed.

Current validation from this pass:

```sh
PYTHONPYCACHEPREFIX=/private/tmp/g4pys-pycache python3 -m py_compile daemon/lyrics_display_daemon.py tools/package_browser_extension.py tests/test_lyrics_display_daemon.py
python3 -m unittest tests.test_lyrics_display_daemon
python3 tools/package_browser_extension.py
cd firmware && idf.py build
```

Those checks pass. Then load `browser_extension/` unpacked in Chrome, open YouTube Music, and watch the daemon logs for an extension connection and frame broadcasts. After flashing firmware, use `/diag/display?pattern=orientation`, `/diag/display?pattern=polarity`, and `/diag/display?pattern=timing` for the real hardware display checks.

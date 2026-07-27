# Synced-Lyrics Display — Design

> Turn the Waveshare ESP32-S3-RLCD-4.2 into a paper-like **synced-lyrics screen**
> for music played on a Mac via YouTube Music **in the browser**. See
> [`spec.md`](./spec.md) for the board.

## The one hardware fact that shaped everything

A normal "Bluetooth speaker" uses **A2DP**, which is a **Bluetooth *Classic*** profile.
The ESP32-**S3** has **BLE only — no Bluetooth Classic → no A2DP** (spec line 22). So this
board **cannot** be a standard Bluetooth speaker. It's a **display**: audio plays on the
Mac; the board only shows lyrics.

## Why browser + extension (not the th-ch desktop app)

The th-ch/youtube-music Electron client is open-source but you'd **log your Google account
into a third-party app**. Since **Nov 2024 Google removed OAuth for YouTube Music**, such
clients use **cookie/session auth** (handing over your live Google session), and the app may
be **unsigned/unnotarized** on macOS. Instead we use a **self-written browser extension**:
your Google login **never leaves your normal browser** — the extension only *reads* the page
you're already logged into. All code is yours.

## Architecture in one line

**YouTube Music plays in your browser → your extension reads song + position → a tiny local
daemon resolves + renders lyrics to a 1-bit bitmap → pushes binary frame envelopes over
WebSocket → the board blits them to its LCD.**

Two boards speak this protocol today: the ESP32-S3 (400×300, the original target) and the
ESP8266 GeekMagic SmallTV (240×240 color TFT, `esp8266/clawdmeter_esp8266/lyrics_stream.cpp`),
which maps 1-bit UI masks to the Tend color palette, streams album-art overlays in
real RGB565, and keeps its on-device text renderer as
the fallback whenever no stream flows.

```
┌───────────────────────────── macOS ─────────────────────────────┐      WiFi / LAN
│  Browser (you're logged into music.youtube.com normally)         │    (mDNS + WebSocket)
│    └─ ★ Extension (content script)                               │          │
│         reads <video>.currentTime / .paused / .duration          │          │
│         + navigator.mediaSession.metadata (title/artist/album)   │          │
│                        │ localhost WebSocket                      │          │
│  ★ Tiny local daemon (your code; launchd auto-start) ◀───────────┘          │
│         ├─ SQLite resolve (manual ▸ lrclib cache)                            │
│         ├─ lrclib fetch on miss                                             │
│         ├─ Core Text render (macOS → 1-bit 400×300)                         │
│         └─ WebSocket SERVER + precise scheduler ── binary frame envelope ──┼──▶ ESP32-S3
└──────────────────────────────────────────────────────────────────┘  (~15 KB)   board
                                                                                   ├─ blit → ST7305 LCD
                                                                                   └─ idle: RTC clock + SHTC3
```

Why the daemon is **not** in the browser: a browser can't be a WebSocket *server*, can't use
SQLite/the filesystem, and — the real killer — **backgrounded tabs throttle timers to ~1 s**,
which would wreck millisecond scheduling whenever the music tab isn't focused. The daemon runs
the un-throttled scheduler and **interpolates** position between the extension's updates, so
timing stays dead-on even when the tab is in the background.

## Decisions (the grilled tree)

| # | Decision | Choice | Why |
|---|---|---|---|
| 1 | Board's role | **Display only** | S3 can't be an A2DP sink; audio stays on the Mac |
| 2 | Music source | **YouTube Music (browser)** | user's player; keeps login in the real browser |
| 3 | Song/position feed | **Self-written browser extension** | reads `<video>.currentTime` + mediaSession; no 3rd-party app holds your Google login |
| 4 | Timing owner | **Thin board, daemon owns timing** | keeps firmware near-stateless |
| 5 | Seamlessness | **Predictive scheduling + interpolation** | daemon pre-sends next line + `swapInMs`; interpolates position so throttled tab updates don't matter |
| 6 | Cache key | **YouTube Music `videoId`** | stable, exact, no re-normalization to look up |
| 7 | lrclib strategy | **`get` then `search` fallback** | precise when strings match, fuzzy rescue otherwise; prefer synced, fall back to plain/instrumental within duration ±3 s |
| 8 | Result handling | **Ladder + TTL negative cache** | synced ▸ plain-static ▸ instrumental ▸ now-playing; re-check "no lyrics" after ~14 d |
| 9 | Overrides | **`source` column, `manual` wins** | hand-fixed lyrics beat the internet and survive cache clears |
| 10 | Transport | **WebSocket + mDNS** | push-based (needed for scheduling); no hardcoded IP |
| 11 | "Daemon" home | **Tiny local daemon, `launchd` auto-start** | browser can't host WS-server/DB/scheduler; your own code; never launch by hand |
| 12 | Payload | **Pre-rendered 1-bit bitmaps** | Mac fonts render Thai/Korean/JP/emoji perfectly; firmware needs zero font/Unicode logic |
| 13 | Idle screen | **Board-local clock + temp** | uses onboard RTC + SHTC3; a clock needs no Unicode |
| 14 | Firmware | **ESP-IDF C++** | native build, stdlib-style WebSocket client, mDNS, and direct ST7305/u8g2 integration |
| 15 | Layout | **Karaoke multi-line** | current line big/bold + context above/below + title·artist header + progress bar |

## Data flow (a song plays)

1. Extension content script detects a track change and reads
   `{ videoId, title, artist, album, durationSec }` + `currentTime`/`paused`;
   streams these to the daemon over **localhost WebSocket** (periodic position ticks +
   immediate `play`/`pause`/`seek`/`track-change` events).
2. Daemon resolves lyrics:
   - `SELECT ... WHERE videoId = ? AND source='manual'` → if present, **use it** (stop).
   - else `... source='lrclib'` cache row → use positive rows; expire negative rows
     after about 14 days.
   - else **fetch lrclib**: try `GET /api/get`; on 404 try `GET /api/search`.
     Search prefers the closest synced candidate within **±3 s**, then falls back to
     the closest plain/instrumental candidate. Store result (or a TTL'd negative row).
3. Daemon parses the LRC into `[{ tMs, text }]`.
4. **Scheduler loop** (un-throttled, in the daemon): it **interpolates** the live position
   from the last extension tick + `playbackRate`, computes the next line's due time,
   **pre-renders** its frame with Core Text/fallback drawing, and sends it with a **relative** `swapInMs`;
   the board arms a one-shot timer to flip at exactly that moment. (Relative delay = no
   Mac↔board clock sync.)
5. On **pause / seek / track-change**, the extension's event reaches the daemon, which
   cancels + reschedules and pushes immediately — the board never scrolls past the audio.

## WebSocket protocol

Two links, both mediated by the daemon:

**Extension → daemon** (daemon is server on localhost):

| Message | Payload |
|---|---|
| `now-playing` | `{ videoId, title, artist, album, durationSec }` |
| `tick` | `{ positionSec, paused, playbackRate }` (periodic; throttled to ~1 s when tab hidden) |
| `event` | `{ type: 'play'|'pause'|'seek'|'ended' }` |

**Daemon → board** (board is client; the ESP32 discovers the daemon via mDNS
`_lyrics._tcp`, the ESP8266 dials back to the Mac whose IP it learned from the
claudemeter daemon's `/usage`/`/nowplaying` HTTP pushes):

A board picks its **render profile** at handshake time with query parameters on the
WebSocket path — `/board` (or unknown sizes) means the default 400×300 e-ink layout;
`/board?w=240&h=240` selects the compact 240×240 layout. On connect the daemon sends a
nonce challenge. The board proves possession of its NVS token, the daemon proves the
same token, and HKDF-SHA256 derives independent client→server and server→client keys.
Only then does the daemon send a protected `hello`; the board answers with protected
`ready`. The daemon persists its UUID/token in `~/.g4pys/lyrics-identity.json` (0600),
while a board persists only the token and last authenticated UUID—never the daemon IP.

Bonjour TXT must contain `proto=2`, `auth=hmac-sha256`, and `uuid=<daemon UUID>`.
The board rejects other advertisements, prefers its remembered UUID and same-subnet
IPv4, and always uses the SRV port plus an address returned on that Bonjour interface.
Legacy proto=1 requires an explicit firmware/daemon migration flag and is never chosen
as a fallback from a failed proto=2 authentication.

Application messages are carried in an authenticated SEC2 record inside one binary
WebSocket message. The fixed header is `SEC2`, version/type/reserved, a monotonically
increasing 64-bit sequence, and a 32-bit payload length; HMAC-SHA256 over header+payload
is appended. Sequence checks reject replay/gaps. `hello`, `ready`, and `clear` use the
text content type; LYR1 framebuffer envelopes use the binary content type. The daemon
renders and caches frames per profile, so mixed fleets stream simultaneously.

Each display update is one binary WebSocket message containing a fixed big-endian header
followed by either a full 1-bpp framebuffer or a byte-aligned dirty rectangle at the
profile's size.

```text
magic[4] = "LYR1"
version u8 = 1
kind u8 = 1 full-now, 2 full-scheduled, 3 rect-now, 4 rect-scheduled
width u16, height u16
x u16, y u16, rectWidth u16, rectHeight u16, rowBytes u16
swapInMs u32
payloadBytes u32
payload bytes
```

`width`/`height` carry the profile size, so a board can reject frames that don't match
what it negotiated. Full frames are `width * height / 8` bytes (`15000` at 400×300,
`7200` at 240×240) in `1bpp-lsb-rowmajor` order. Rect payloads contain
`rowBytes * rectHeight` bytes and are byte-aligned on `x`. Scheduled frames/rects are
staged and flipped after `swapInMs`; `*-now` kinds are applied immediately. The ESP32
still accepts the older JSON-header plus binary-payload format as a compatibility
fallback, but the daemon sends only the single-message envelope.

Every board owns a bounded outbound queue and writer task. Persistent backpressure
disconnects that board only. Both peers ping every 20 seconds and require a pong within
10 seconds. Wi-Fi/IP changes, daemon restarts, and reconnects discard the endpoint and
begin fresh Bonjour discovery with jittered exponential backoff capped at 30 seconds.

## SQLite schema (draft, Mac-side, in the daemon)

```sql
CREATE TABLE lyrics (
  video_id     TEXT NOT NULL,
  source       TEXT NOT NULL,          -- 'manual' | 'lrclib'
  synced       INTEGER NOT NULL,       -- 1 = has LRC timestamps, 0 = plain/none
  instrumental INTEGER NOT NULL DEFAULT 0,
  lrc          TEXT,                   -- synced LRC or plain text; NULL = negative
  title        TEXT, artist TEXT, album TEXT, duration_sec INTEGER,
  fetched_at   INTEGER,                -- unix; drives the ~14-day negative-cache TTL
  PRIMARY KEY (video_id, source)
);
```
Resolution priority: `manual` row → positive `lrclib` row or unexpired negative row →
network. A "clear cache" deletes only `source='lrclib'`. Importing a folder of `.lrc`
files writes `manual` rows.

## Browser extension responsibilities

- Content script on `music.youtube.com`: read `<video>.currentTime` / `.paused` /
  `.duration` and `navigator.mediaSession.metadata`; detect track changes.
- Maintain a localhost WebSocket to the daemon; send `now-playing` / `tick` / `event`.
- **No** rendering, DB, or lrclib calls here — those move to the daemon to dodge MV3
  service-worker termination and hidden-tab timer throttling. Keep logic in the content
  script (lives with the tab), not the MV3 service worker.
- Permissions: host access to `music.youtube.com`; localhost HTTP host permissions are
  currently kept as compatibility entries and should be verified in Chrome unpacked mode.

## Local daemon responsibilities (your code; `launchd` auto-start)

- WebSocket server for the extension (localhost) **and** for the board (LAN); advertise
  `_lyrics._tcp` via mDNS/Bonjour.
- SQLite resolve (`manual` ▸ `lrclib` cache ▸ network) + lrclib `get`→`search`.
- Core Text render → 1-bit karaoke frames (system fonts = any language), with a
  geometry-only fallback when PyObjC is unavailable. Two layouts: the 400×300 e-ink
  original and a compact 240×240 profile for the ESP8266 boards; frame caches,
  dirty-rects, and scheduled swaps are all kept per profile.
- Precise scheduler with position interpolation; pre-send + `swapInMs`; pause/seek handling.
- Ships as a per-user **LaunchAgent** (`RunAtLoad` + `KeepAlive`) — starts at login, restarts
  on crash, never launched by hand. Tolerates "extension not connected yet"; the board
  falls back to its local idle screen when the daemon connection is absent.

## Board firmware (ESP-IDF C++) responsibilities

- Bring-up ST7305 (reuse Waveshare demo), WiFi, `ESPmDNS` resolve of the daemon.
- Minimal WebSocket client → receive `LYR1` frame envelopes → blit or patch rects; honor
  `swapInMs` timer.
- **Idle mode** (no daemon connection): read PCF85063 RTC + SHTC3, draw a clock +
  temp/humidity with a tiny built-in digits/Latin font (no Unicode needed).
- Auto-reconnect; send `ready` on (re)connect to trigger a resync.

## ESP8266 client (`esp8266/clawdmeter_esp8266/lyrics_stream.cpp`)

The GeekMagic SmallTV port shares the protocol but not the constraints — it is an
Arduino sketch on a much smaller chip, so the client is shaped differently:

- Hand-rolled non-blocking WebSocket client over `WiFiClient`, pumped from `loop()`
  (byte-wise state machine; a 7.2 KB frame arriving across TCP segments never blocks
  the web server or the other screens). No extra library, no task, no timer ISR.
- **No mDNS query and no configuration**: the Mac already POSTs `/usage` and
  `/nowplaying` to the device, so the sketch remembers the source IP of those pushes
  and dials back to `:8766/board?w=240&h=240&color=rgb565`. Connects only while the MUSIC screen is
  visible; a stale host (no push in 10 min) is never dialed.
- Frames blit as Tend-palette RGB565 rows: stable bands in the compact layout
  colorize the 1-bit UI mask. The daemon follows an overwritten cover region with an
  `ART1` big-endian RGB565 rectangle, streamed one scanline at a time so even a
  full-screen cover needs no 115 KB color framebuffer. One 7.2 KB mono framebuffer is
  malloc'd while streaming, and the on-device marquee canvases (~17 KB) are freed while
  the stream owns the panel, so the two render paths never hold heap at once.
- The on-device Thai/Latin text renderer remains the fallback: stream drops, daemon
  `clear`, or the Mac going away all hand the panel back within seconds.
- Sends no board token — leave `G4PYS_LYRICS_BOARD_TOKEN` unset when ESP8266 boards
  should connect.

## Deferred / open (intentionally, not blockers)

- Battery/sleep behavior on the 18650 (reflective LCD is already very low power).
- Exact idle-screen design (clock face, weather layout).
- Full hardware validation of display polarity/orientation/refresh timing.
- Optional per-syllable scheduling. Enhanced LRC syllable markers drive the highlight
  progress, but pre-send scheduling is still line-boundary based.
- Chrome/Firefox packaging edge cases. The MV3 manifest currently keeps localhost HTTP
  host permissions; verify by loading unpacked, removing those entries, and checking that
  the content-script WebSocket still connects.

## Suggested build order

1. **Firmware skeleton**: ST7305 up + blit a hardcoded 400×300 bitmap over USB.
2. **WiFi + mDNS + WebSocket client**: receive a framebuffer from a throwaway Python
   script and blit it.
3. **Daemon scaffold**: Python process, WS server for the board, mDNS advertise;
   render a static karaoke frame with Core Text/fallback drawing and push it.
4. **Extension**: content script reads song + position, streams to the daemon; log it.
5. **Resolution**: lrclib `get`→`search` + SQLite cache + `manual` override.
6. **Scheduler**: interpolation + pre-send + `swapInMs`; handle pause/seek/track-change.
7. **Idle mode** on the board (RTC + SHTC3 clock) + `launchd` LaunchAgent for the daemon.
8. Polish: reconnect resync, negative-cache TTL, `.lrc` import, dirty-rect.

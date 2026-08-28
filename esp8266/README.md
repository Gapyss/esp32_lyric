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
lyrics daemon (esp8266/daemon/lyrics_display_daemon.py --insecure)
   ├── WebSocket :8766 ──1-bit UI + RGB565 cover art──> ESP8266 lyrics screen
   └── HTTP GET /usage every 60s ─────────────────────> ESP8266
                                        (teaches it the Mac's IP; the board
                                         dials :8766 back on its own)
```

### The screen

The lyrics screen is rendered **entirely on the Mac** by
`esp8266/daemon/lyrics_display_daemon.py`: a Tend-color 240×240 UI with Core Text Thai
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

### 4. Start the Mac daemon

One process, from this directory:

```sh
python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure
```

That is the whole Mac side. It renders the frames, serves the board on `:8766`,
and knocks on the board's `/usage` every 60s so it learns where the Mac is.

**Why the knock?** The board is never told the Mac's address — it *learns* it
from the source IP of a request to `/usage`, then dials that IP back on port
8766. It also forgets a Mac it hasn't heard from in 10 minutes
(`LYR_HOST_FRESH_MS` in `lyrics_stream.cpp`), which is why the knock repeats
instead of firing once at startup. The `t=` parameter it carries is what drives
the clock on the waiting screen.

If your board answers to something other than `clawdmeter.local`:

```sh
python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure \
  --announce-url http://192.168.1.35
```

`--no-announce` turns the knock off, for when something else is already pushing
to the board's `/usage`.

> **This used to need `claudemeter_daemon.py` too.** That daemon polls Claude
> usage and has nothing to do with lyrics; it was only ever here because its
> HTTP pushes happened to teach the board the Mac's IP. The daemon now does its
> own knock, so this board no longer depends on it. Nothing on this board draws
> a usage screen — if you still want the dashboard's meters populated, running
> `claudemeter_daemon.py` alongside remains optional and harmless.

**Why `--insecure`?** By default the daemon creates an identity and requires a
mutual HMAC-SHA256 handshake (secure proto=2) — the scheme the ESP32 e-ink
board uses. The ESP8266 lacks the heap to buffer-and-verify a whole SEC2 record
on top of its framebuffer, so it speaks the unauthenticated proto=1 instead.
`--insecure` drops the identity, advertises `proto=1`, and streams unwrapped
frames this board can render. (Or set `G4PYS_LYRICS_INSECURE=1`.) Without it the
board can't complete the handshake and the panel stays on the waiting screen.

There is no pairing step and no token on this link — the trade is LAN-link
authentication for ~7 KB of heap, so run it on a trusted home network.

> **Running this alongside the ESP32 e-ink board.** They are separate daemons
> now (`daemon/` serves the e-ink board, `esp8266/daemon/` serves this one), but
> they still default to the same ports and the same mDNS name. This fork
> defaults to `--no-mdns` so it won't contest the `_lyrics._tcp` instance the
> e-ink board browses for — Bonjour gives a contested name to the oldest holder,
> which would otherwise feed the e-ink board this fork's `proto=1` record and
> break its handshake. To run both at once, also move this one off the shared
> ports and point its copy of the extension at the new one:
>
> ```sh
> python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure \
>   --extension-port 8775 --board-port 8776
> ```
>
> then change `WS_URL` in `esp8266/browser_extension/content_script.js` to
> `ws://127.0.0.1:8775/extension`. Otherwise, run one or the other.

### 5. Play something

Load the browser extension (`esp8266/browser_extension/` → `chrome://extensions`
→ Developer mode → Load unpacked) and play a track in **YouTube Music**. It feeds
title/artist/position to the lyrics daemon.

Within a few seconds the panel switches from the waiting screen to the streamed
frames. **If it stays on the waiting screen**, the screen itself tells you which
half is missing:

| The panel says | What it means | Fix |
|---|---|---|
| `waiting for lyrics` | No Mac IP known yet, or the daemon is connected but idle between tracks | Check the daemon logged `board announce reached …`. If it logged a failure, the board isn't resolving — pass `--announce-url http://<board-ip>` |
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

Everything this board needs lives under `esp8266/`. Nothing outside it is
required at runtime.

```
esp8266/
├── clawdmeter_esp8266/
│   ├── clawdmeter_esp8266.ino   # WiFi + HTTP API + dashboard + the waiting screen
│   ├── tend.h                   # shared Tend palette + the .ino/stream contract
│   ├── lyrics_stream.cpp        # the lyrics screen: WS client for the lyrics daemon
│   └── index_html_gz.h          # generated — gzipped dashboard served at /
├── daemon/
│   └── lyrics_display_daemon.py # renders the screen; knocks on /usage. The Mac side.
├── browser_extension/           # feeds YouTube Music playback to the daemon
├── tests/
│   └── test_lyrics_display_daemon.py
└── tools/
    └── gen_index_gz.py          # regenerate index_html_gz.h after editing INDEX_HTML
```

Run the tests from the repo root:

```sh
python3 -m unittest discover -s esp8266/tests -t .
```

### This daemon is a fork, not a shared module

`esp8266/daemon/lyrics_display_daemon.py` is a copy of
`daemon/lyrics_display_daemon.py`, not an import of it. The ESP32 e-ink board
still dials that other copy with the SEC2 handshake
(`firmware/main/board_client.cpp`), and this tree has to stand alone so it can
be split into its own repository. **A fix that matters to both boards has to be
applied twice** — that is the accepted cost of the split.

The two copies have already diverged in three ways, all in this one:

| | `daemon/` (ESP32 e-ink) | `esp8266/daemon/` (here) |
|---|---|---|
| Board learns the daemon's address by | browsing `_lyrics._tcp` over mDNS | the `/usage` knock (`BoardAnnouncer`) |
| mDNS advertising | on | **off** by default (`--mdns` opts in) |
| Needs `claudemeter_daemon.py` | no | no (it used to) |

### Splitting this into its own repo

`esp8266/` is a clean subtree boundary, so history comes with it:

```sh
git subtree split -P esp8266 -b esp8266-only
```

Two things to fix in the new repo afterwards, neither of which blocks the split:

- paths lose their `esp8266/` prefix (`esp8266/daemon/…` → `daemon/…`), so the
  commands in this README and the `-s esp8266/tests` in the test invocation
  shorten;
- the 400×300 ESP32 e-ink render path (`WIDE_PROFILE`, `_draw_wide`,
  `WIDE_PROGRESS_GEOM`, its cover placement) and the whole SEC2/identity/pairing
  layer become dead code there, since this board uses neither. They are still
  present and still work; deleting them is a separate cleanup, and doing it
  *after* the split keeps this repo's e-ink board unaffected.

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

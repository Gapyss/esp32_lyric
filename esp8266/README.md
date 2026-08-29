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

**The WiFi watchdog.** `wm.autoConnect()` runs exactly once, in `setup()`, so
until recently nothing re-read `WiFi.status()` afterwards: an association lost
*after* boot parked the panel on `wifi lost` until someone power-cycled the box.
`wifiWatchdogTick()` in `loop()` now closes that. Two stages, deliberately far
apart (`WIFI_WATCHDOG_*` at the top of the sketch):

| Down for | What happens |
|---|---|
| 30s | `WiFi.reconnect()`, repeated every 30s. Cheap and non-destructive — it does not clear credentials |
| 10 min | `ESP.restart()`, which lands back in `setup()` and retries the saved network first |

The 10-minute gap is the load-bearing number. A reboot lands in
`wm.autoConnect()`, which *blocks* in the captive portal for
`WIFI_PORTAL_TIMEOUT_S` (3 min) and then reboots again — so restarting eagerly
would turn a fifteen-minute router outage into a hotspot loop that also puts
`/update` out of reach. A screen reading `wifi lost` while the router comes back
is the better failure.

Two details that are easy to get wrong if you touch this:

- The tick sits **below** the `if (otaInProgress) return;` line in `loop()`. A
  WiFi hiccup mid-upload must never fire `reconnect()` — let alone
  `ESP.restart()` — while `Update.write()` has the flash open. That is the same
  reset-mid-flash hazard `backlightStopForFlash()` exists to prevent, on the one
  operation that cannot be retried. Both OTA exit paths re-arm `wifiOkMs`, which
  is otherwise as stale as the transfer was long.
- Regaining the association calls `MDNS.notifyAPChange()`. Without it
  `clawdmeter.local` stays dark for the rest of the session even though the box
  answers on its IP — and that name is what the daemon's announce knock
  resolves, so a silent SDK reconnect would still cost you the stream.

**Reading the watchdog from the Mac.** None of its `Serial.println()` output is
reachable — this board is updated over the air with no USB attached — and
`ESP.getResetReason()` reports `Software/System restarted` for a watchdog reboot,
`/restart` and `/wifi-reset` alike. So `/usage.json` carries three fields, and
`tools/lyrics.sh status` prints them:

| Field | Meaning |
|---|---|
| `wifidrops` | Associations lost since boot. Climbing steadily = a flapping link |
| `wifidown` | Seconds since the association dropped; `0` while connected |
| `mdnsok` | `0` if the last `notifyAPChange()` failed — `.local` may be dark, fall back to `--announce-url http://<ip>` |

A `rst` of `Software/System restarted` with a low `up` **and** a non-zero
`wifidrops` is the signature of a watchdog reboot rather than someone hitting
`/restart`.

One deliberate hole: any connected tick resets the clock, so a link that
associates for a moment every 25 seconds keeps resetting it and never reaches
the restart stage — `reconnect()` just fires forever. That is the intended
trade (a flapping link is still a link, and rebooting into a 3-minute blocking
portal would make it worse), and `wifidrops` is what makes it visible.

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

```sh
esp8266/tools/lyrics.sh
```

That is the whole Mac side. It renders the frames, serves the board on `:8766`,
and knocks on the board's `/usage` every 60s so it learns where the Mac is.

The script is a wrapper around one `python3` invocation, and it exists because
three mistakes are always one keystroke away and all three produce the *same*
useless symptom — a panel stuck on `waiting for lyrics` with nothing to say why.
It refuses to start a second daemon on top of a running one, always passes
`--insecure`, and reports what the board itself thinks before and after:

```
pre-flight
  + clawdmeter.local resolves to 192.168.1.35
  + board wifi GE v2 (-46 dBm) - streaming - heap 26k - up 0h33m - boot External System

starting
  + daemon up (pid 51114), logging to ~/Library/Logs/g4pys-lyrics-display.log

waiting for the board
  lyrics are on the panel.
```

It reports `lyrics are on the panel` in about 8 seconds, or tells you which link
in the chain is broken. Two other verbs:

```sh
esp8266/tools/lyrics.sh status   # daemon up? extension attached? board streaming?
esp8266/tools/lyrics.sh stop
esp8266/tools/lyrics.sh start -f # foreground, if you want the output live
```

Everything after `start` is passed through to the daemon, so
`esp8266/tools/lyrics.sh start --announce-url http://192.168.1.35` works. The
underlying command is unchanged and still fine to run directly:

```sh
python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure
```

> Run it that way and stdout is block-buffered into whatever you redirect to,
> so `board announce reached …` and `board connected` will not appear in a log
> file until the buffer fills. The script sets `PYTHONUNBUFFERED=1` for exactly
> this reason — the log is the only record of why a run failed.

**Why the knock?** The board is never told the Mac's address — it *learns* it
from the source IP of a request to `/usage`, then dials that IP back on port
8766. It also forgets a Mac it hasn't heard from in 10 minutes
(`LYR_HOST_FRESH_MS` in `lyrics_stream.cpp`), which is why the knock repeats
instead of firing once at startup. The `t=` parameter it carries is what drives
the clock on the waiting screen.

The announcer resolves `clawdmeter.local` **once**, with `gethostbyname`, and
knocks the numeric address from then on. That is not premature tidying: the
board publishes an A record and no AAAA, so the `getaddrinfo` that `urlopen`
would otherwise do sits out the full mDNS IPv6 timeout before handing back the
address it already had — measured at 5.01s per knock against 0.00s for
`gethostbyname`. Worse, `urlopen`'s `timeout` covers socket operations but *not*
name resolution, so `BOARD_ANNOUNCE_TIMEOUT_SECONDS` never bounded that wait.
A failed knock drops the cached address, so a board that takes a new DHCP lease
is picked up on the next cycle.

If your board answers to something other than `clawdmeter.local`:

```sh
python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure \
  --announce-url http://192.168.1.35
```

`--no-announce` turns the knock off, for when something else is already pushing
to the board's `/usage`.

> There is no launchd plist pointing at `esp8266/daemon/`, by choice — the
> daemon runs when you start it. After editing it, `tools/lyrics.sh stop` then
> `tools/lyrics.sh` restarts it; `stop` keys off whatever holds `:8766` and
> refuses to kill a process that isn't a lyrics daemon.
>
> The repo's older `clear_lyric_daemon.sh` still works but is the blunt version:
> its `lyrics_display_daemon.py` pattern matches the ESP32 daemon too, so it
> stops both, and it lives outside `esp8266/` so it does not come along in a
> split. Prefer `tools/lyrics.sh stop`.

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
> break its handshake.
>
> To run both at once, move the **ESP32** daemon, not this one. This board dials
> a hardcoded port (`#define LYR_PORT 8766` in `lyrics_stream.cpp`) and there is
> no way to tell it otherwise — it only ever learns an *address*, never a port.
> The e-ink board reads its port out of the mDNS SRV record instead
> (`r->port ? r->port : LYRICS_DAEMON_PORT`, `firmware/main/board_client.cpp`),
> so it is the one that can move:
>
> ```sh
> # this board — unchanged, on the ports it insists on
> python3 esp8266/daemon/lyrics_display_daemon.py serve --insecure
>
> # the ESP32 e-ink daemon — moved out of the way
> python3 daemon/lyrics_display_daemon.py serve --extension-port 8775 --board-port 8767
> ```
>
> Then point the *root* `browser_extension/content_script.js` at the moved
> daemon (`WS_URL` → `ws://127.0.0.1:8775/extension`) and load it as a second
> unpacked extension; `esp8266/browser_extension/` stays on 8765 for this board.
> Otherwise, run one or the other.

### 5. Play something

Load the browser extension (`esp8266/browser_extension/` → `chrome://extensions`
→ Developer mode → Load unpacked) and play a track in **YouTube Music**. It feeds
title/artist/position to the lyrics daemon.

Within a few seconds the panel switches from the waiting screen to the streamed
frames. **If it stays on the waiting screen**, the screen itself tells you which
half is missing:

| The panel says | What it means | Fix |
|---|---|---|
| `waiting for lyrics` | No Mac IP known yet, or the daemon is connected but idle between tracks | `tools/lyrics.sh status`. If it logged a knock failure, the board isn't resolving — pass `--announce-url http://<board-ip>` |
| `reaching the lyrics daemon…` | Mac IP known, the socket won't come up | `tools/lyrics.sh start`; check nothing is blocking `:8766` between Mac and board |
| Streamed frames never appear, but the socket connects | The daemon is demanding a proto=2 handshake this board can't do | `tools/lyrics.sh status` flags a daemon running without `--insecure`; restart it with the script |
| `wifi lost` / `reconnecting` in place of the IP | The board dropped its association | Wait. The watchdog calls `WiFi.reconnect()` after 30s and keeps retrying; if the network is still gone after 10 minutes it reboots into `setup()`, which retries the saved network and only opens the setup hotspot if that fails too |

`tools/lyrics.sh status` answers the same question from the Mac side, and covers
the one case the panel cannot show you — that the daemon is up but running in
the wrong mode.

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

Everything this board needs lives under `esp8266/`. The one piece of state that
sits outside it is the lyrics cache at `~/.g4pys/lyrics-display.sqlite3`
(`--db` overrides it) — deliberately, so both boards share one cache and neither
re-fetches from lrclib after the other already has. It is created on demand, so
a fresh clone needs nothing seeded.

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

# Running the ESP8266 Lyrics Board from Windows

This guide sets up the **clawdmeter** ESP8266 lyrics board with a Windows 10/11
PC doing the rendering. It replaces step 4 of `esp8266/README.md` for people not
on a Mac; everything before it (flashing the board, joining it to WiFi) and
after it (playing something) is identical and lives in that README.

The board itself does not care which operating system feeds it. It dials a PC
on TCP 8766 and paints whatever framebuffer arrives.

## What actually runs where

```text
Chrome/Edge on music.youtube.com          ESP8266 board
  browser extension                          (240x240 panel)
        |                                          ^
        | ws://127.0.0.1:8765                      | ws  :8766
        v                                          |  (board dials IN)
     lyrics_display_daemon.py  ---- renders every frame ----+
     (this PC)
```

The PC draws the entire screen — text, karaoke fill, album art — and streams
finished 1-bit frames. There is no on-device now-playing renderer, so **when the
daemon is not running the panel falls back to its waiting screen**. That is
normal, not a failure.

Two consequences worth knowing before you start:

- **The source is YouTube Music** (`music.youtube.com`). A video playing on
  youtube.com is not picked up, on any operating system.
- **The board connects to you, not the other way round.** That is why Windows
  Firewall matters more here than anything else on this page.

### Why Windows renders differently from macOS

On a Mac the daemon draws with Core Text and the system's Sukhumvit Set font.
Neither exists on Windows, so it switches to a Pillow renderer that draws the
identical layout: HarfBuzz shapes Thai, FreeType rasterizes the glyphs, and the
ICU that Windows already ships (`icu.dll`) finds Thai word breaks. The fonts
travel with the repo (`esp8266/daemon/assets/fonts/`, IBM Plex Sans Thai + IBM
Plex Mono, OFL), so there is nothing to install by hand.

The layout is the same. The typography is not quite: IBM Plex Sans Thai is a
little wider than Sukhumvit Set, so a long Thai line may wrap one word earlier
than it does on a Mac.

## Setup

### 1. Install Python 3.10 or newer

From [python.org](https://www.python.org/downloads/windows/), ticking **Add
python.exe to PATH**. The `py` launcher is included and is what `lyrics.ps1`
prefers.

3.10 is a hard floor: the renderer wheels publish nothing older.

> Windows keeps a `python.exe` placeholder on `PATH` that only opens the
> Microsoft Store. If typing `python` opens the Store, Python is *not*
> installed — the real installer is the one above.

### 2. Install the renderer packages

```powershell
py -3 -m pip install -r esp8266\daemon\requirements-windows.txt
```

Three packages (Pillow, uharfbuzz, freetype-py). Each ships its native library
inside the wheel, so there is no separate FreeType or HarfBuzz to install.

Without them the daemon still runs, but falls back to a geometry-only renderer
that draws rules and the progress bar and **no text at all**. `lyrics.ps1`
refuses to start in that state rather than let you debug a blank panel.

### 3. Load the browser extension

`chrome://extensions` → enable **Developer mode** → **Load unpacked** →
`esp8266\browser_extension\`. Edge works the same way at `edge://extensions`.

> Use `esp8266\browser_extension\`, not the `browser_extension\` at the repo
> root — that one belongs to the ESP32 e-ink board and talks to a different
> daemon. Running both at once fights over port 8765.

### 4. Start the daemon

```powershell
powershell -ExecutionPolicy Bypass -File esp8266\tools\lyrics.ps1
```

`lyrics.ps1` is the PowerShell twin of `lyrics.sh`, with the same verbs:

| Command | Does |
|---|---|
| `lyrics.ps1` or `lyrics.ps1 start` | Pre-flight, then start in the background |
| `lyrics.ps1 start -f` | Start in the foreground (Ctrl+C stops it) |
| `lyrics.ps1 status` | What the daemon and the board each think |
| `lyrics.ps1 stop` | Stop the daemon holding `:8766` |

Anything else you pass is handed to the daemon, so
`lyrics.ps1 start --announce-url http://192.168.1.42` works.

It always passes `--insecure`. That is not optional for this board: the ESP8266
lacks the heap to buffer and verify a SEC2 record on top of its framebuffer, so
it speaks unauthenticated proto=1. The trade is LAN-link authentication for
about 7 KB of heap — run it on a network you trust.

### 5. Allow the firewall prompt

The **first** start raises a Windows Firewall prompt. Allow **Private
networks**. If you dismissed it, add the rule from an administrator PowerShell:

```powershell
New-NetFirewallRule -DisplayName "Clawdmeter lyrics" -Direction Inbound -Protocol TCP -LocalPort 8766 -Action Allow -Profile Private
```

Also check the WiFi network is set to **Private**, not **Public** — a Public
profile blocks the rule even once it exists.

## What a healthy start looks like

```text
pre-flight
  + Python 3.12
  + renderer packages installed
  + clawdmeter.local resolves to 192.168.1.42
  ! board wifi MyNetwork (-52 dBm) - NOT connected (no PC address known) - heap 24k - up 0h03m - boot Power On

starting
  + daemon up (pid 12345), logging to C:\Users\you\AppData\Local\g4pys\lyrics-display.log
    the first start may raise a Windows Firewall prompt: allow Private networks, or the board cannot dial in

waiting for the board
  + board wifi MyNetwork (-52 dBm) - connected, idle (nothing playing) - heap 24k - up 0h03m - boot Power On

  board is connected. Play something and the panel follows.
```

The red `!` at pre-flight is expected: the board has not been told where this PC
is yet, and the announcer has not had its first turn. The board going from
**NOT connected** to **connected, idle** is the moment that matters.

The log itself (`%LOCALAPPDATA%\g4pys\lyrics-display.log`) opens with:

```text
rendering with Pillow (HarfBuzz + FreeType)
extension WebSocket: ws://127.0.0.1:8765/extension
board WebSocket: ws://0.0.0.0:8766/board
board WebSocket auth: DISABLED (--insecure): proto=1, unauthenticated
board announce: http://clawdmeter.local/usage every 60s
board announce reached http://clawdmeter.local at 192.168.1.42 (it now knows this Mac's address)
board connected (240x240, proto=1)
extension connected
```

Those last two lines are the pair to look for. `board announce reached` without
`board connected` is the firewall signature — see below.

## Troubleshooting

### Pre-flight refuses to start

| Message | Means |
|---|---|
| `'py -3' did not run -- Python is not installed` | No interpreter. If `python` opens the Microsoft Store, that is the placeholder, not an install. Or set `G4PYS_LYRICS_PYTHON` to a real one. |
| `Python 3.9 is too old -- the renderer packages need 3.10 or newer` | Python is there but predates the wheels. Installing a newer one alongside is fine; `py -3` picks the newest. |
| `the Pillow renderer's packages are missing -- the panel would show no text` | Python is fine, step 2 has not run. The message prints the exact `pip` line. |
| `already running: pid N holds :8766` | A daemon is already up. `lyrics.ps1 status`, or `stop` first. |

### The panel is stuck on "waiting for lyrics"

Almost always the firewall. The knock goes *out* from the PC fine, so the board
learns the address; the board's dial *back in* is what gets dropped. The log
signature is exact:

```text
board announce reached http://clawdmeter.local at 192.168.1.42 ...
                          ... and never a "board connected" line
```

Do step 5. If the rule already exists, confirm the WiFi network is Private.

Other causes, in the order worth checking:

1. **The board is on another network** (a 5 GHz SSID the ESP8266 cannot join, or
   a guest network with client isolation). `lyrics.ps1 status` reports the
   board's own SSID and RSSI.
2. **Nothing is playing.** `connected, idle (nothing playing)` is a healthy
   board — open music.youtube.com and press play.
3. **The extension is not attached.** `status` says
   `nothing attached on :8765 -- open music.youtube.com` when the tab is closed.

### `cannot resolve clawdmeter.local`

Windows resolves `.local` names over mDNS, and it is the flakiest part of this
setup. Skip the name entirely:

```powershell
$env:G4PYS_LYRICS_BOARD_NAME = "192.168.1.42"
powershell -ExecutionPolicy Bypass -File esp8266\tools\lyrics.ps1 start --announce-url http://192.168.1.42
```

Find the board's address from your router's DHCP table, or from the board's own
dashboard if you can already reach it. Giving it a DHCP reservation makes this
permanent.

### The panel shows a layout but no text

The geometry-only fallback renderer is drawing. It means the daemon started
without the Pillow packages — the log says so:

```text
Pillow renderer unavailable (...); using geometry-only fallback (rules/progress only, no text)
```

Re-run step 2. Starting through `lyrics.ps1` prevents this; the direct
`py -3 ... serve --insecure` command does not.

### Thai lyrics wrap in odd places

The daemon could not load Windows' ICU, so it has no Thai word dictionary. The
log says `ICU unavailable; lyrics wrap at spaces only`. It then wraps at spaces,
and a run with no space in it gets split wherever it stops fitting — mid-word.
Latin lyrics are unaffected, since their spaces already mark the breaks.

This should not happen on a current Windows: 1903 and later export `icu.dll`,
and 1703-1809 export the same functions as `icuuc.dll`. The daemon tries both.

### `--font` appears to do nothing

Correct — it names a Core Text family and only the macOS renderer reads it. The
Pillow renderer uses the vendored fonts and now says so at startup.

## Reference

### Where things live

| | Path |
|---|---|
| Log | `%LOCALAPPDATA%\g4pys\lyrics-display.log` |
| Lyrics cache | `%USERPROFILE%\.g4pys\lyrics-display.sqlite3` |
| Fonts | `esp8266\daemon\assets\fonts\` |

### Environment variables

| Variable | Default | Use |
|---|---|---|
| `G4PYS_LYRICS_PYTHON` | `py -3`, else `python` | Pick a specific interpreter |
| `G4PYS_LYRICS_BOARD_NAME` | `clawdmeter.local` | The board's address, for `status` |
| `G4PYS_LYRICS_LOG` | `%LOCALAPPDATA%\g4pys\lyrics-display.log` | Move the log |
| `G4PYS_LYRICS_ANNOUNCE_URL` | `http://clawdmeter.local` | Where to knock |
| `G4PYS_LYRICS_RENDERER` | `auto` | Force `coretext`, `pillow` or `fallback` |
| `G4PYS_LYRICS_BOARD_PORT` | `8766` | Move the board port |
| `G4PYS_LYRICS_EXTENSION_PORT` | `8765` | Move the extension port |

### Without the script

```powershell
py -3 esp8266\daemon\lyrics_display_daemon.py serve --insecure
```

This skips every guard `lyrics.ps1` adds — the port check, the forced
`--insecure`, and the renderer pre-flight. Useful for reading startup output
directly; not the way to run it day to day.

### Running the tests

```powershell
py -3 -m unittest discover -s esp8266\tests -t .
```

The Pillow renderer's tests skip unless its packages are installed, so run this
from the same environment as step 2 to actually exercise them.

## Known rough edges

- **`lyrics.ps1` is developed on macOS**, where no PowerShell exists to run it.
  Its logic is reviewed rather than executed, so a message that reads oddly is
  worth reporting rather than working around.
- **Ctrl+C in foreground mode** can be slow to take on Windows' proactor event
  loop. `lyrics.ps1 stop` from another window is the reliable lever.
- **mDNS** (`clawdmeter.local`) is the least reliable piece. A DHCP reservation
  plus `G4PYS_LYRICS_BOARD_NAME` removes the dependency entirely.

# g4pys Lyrics Display Bridge

Build a store/upload zip:

```sh
python3 tools/package_browser_extension.py
```

Optional Firefox unlisted signing, after installing `web-ext` and exporting
`WEB_EXT_API_KEY` / `WEB_EXT_API_SECRET`:

```sh
python3 tools/package_browser_extension.py --firefox-sign
```

Chrome Web Store signing is performed by the store during upload. For local
Chrome development, load this directory unpacked.

## Files

| File | Job |
|---|---|
| `content_script.js` | Runs on music.youtube.com; feeds track/position to the daemon over `ws://127.0.0.1:8765`. Also writes a `daemonHeartbeat` timestamp to `chrome.storage.session` every 30s while that socket is open |
| `background.js` | Service worker. A `chrome.alarms` tick every minute knocks on the board's `/usage` so it learns this Mac's address — a backup for the daemon's own knock |
| `popup.js` / `popup.html` | Theme, board address, and the Status panel |

## The backup knock

The ESP8266 board is never configured with the daemon's address: it learns it
from the *source IP* of a request to `/usage` and dials that IP back on 8766.
Normally `BoardAnnouncer` in the daemon does that. `background.js` is a second,
independent path, worth having because the two fail for different reasons — the
daemon knocks `clawdmeter.local`, so a quiet mDNS responder takes it out, while
this one knocks the numeric IP saved in the popup and ignores mDNS entirely.

Three things that are easy to get wrong here:

- **It cannot live in the content script.** A content script on
  `https://music.youtube.com` cannot fetch `http://192.168.1.35` — blocked as
  mixed content *and* by Private Network Access — and it fails silently, which
  is the worst outcome for something whose whole job is reliability. Only
  extension-origin requests with host permissions get through.
- **The heartbeat must be periodic, not edge-triggered.** The daemon socket
  stays open for hours, so a timestamp written only on open/close would be
  hours stale and the worker's 90s freshness test would never pass — the knock
  would silently never fire. It also has to live in `chrome.storage`, never in
  a worker global: MV3 tears the worker down after ~30s idle, so the alarm
  always wakes a fresh one. `storage.session` rather than `storage.local` — it
  is liveness, not settings, and memory-backed suits a value that should not
  outlive the browser. The worker raises its access level at startup so a
  content script (an untrusted context) may write it.
- **It knocks `/usage`, never `/usage.json`.** Only `/usage` calls
  `lyricsStreamNoteHost()`, and that asymmetry is deliberate in the firmware:
  `/usage.json` is polled by the board's own dashboard from any browser, so
  teaching it the caller's address would let a phone opening that page redirect
  the stream at the phone.

Two guards stop the knock from doing harm. It only fires when a fresh heartbeat
proves a daemon socket is open **on this machine** (otherwise a laptop running
this extension without a daemon would point the board at a host with nothing
listening), and only when the optional `http://*/*` permission has actually been
granted by clicking Connect.

This extension is shared verbatim between the two board trees (`browser_extension/`
and `esp8266/browser_extension/` are byte-identical on purpose; change one and
copy it across). The ESP32 e-ink board serves
`/usage.json` but no `/usage` — it browses `_lyrics._tcp` over mDNS and never
needed a knock — so a 404 there is reported as "not this kind of board", not as
a failure.

## Firmware target

The popup has a Firmware section for the board IP/host and optional MAC
address. The Connect button requests permission for that board origin and checks
`http://<ip-or-host>/usage.json`. The Status panel above it shows, in one
glance, whether the daemon is connected, whether the last knock landed, and what
the board itself reports (`lyr`, wifi, heap, uptime, boot reason, and the
watchdog's `wifidrops` / `wifidown`).

Browsers connect to the firmware by IP/host. The MAC value is stored with the
extension settings for identification; it is not used as a network transport
address.

# Thai Text Rendering: headless-Chrome bitmap pipeline

**Status:** design draft, not yet implemented. Supersedes PLAN.md §7a (font selection),
§9 (Thai combining-mark risk), and §11 (Tahoma font conversion) — those describe rendering
Thai with on-device u8g2 fonts, which this document replaces.

---

## 1. Problem

u8g2 draws UTF-8 but does **no complex text shaping** (no GSUB/GPOS). Thai combining marks
(vowels, tone marks) need shaping to stack correctly over base consonants — without it,
marks fall off or land in the wrong place. PLAN.md's original mitigation (Tahoma converted
to a u8g2 bitmap font, relying on its marks having zero advance width) is a partial fix, not
a real one.

A proper text-shaping engine (e.g. a browser's layout engine, via headless Chrome) renders
Thai correctly. The ESP32 can't run a browser, so shaping has to happen off-device and cross
the wire as a bitmap, not as text.

---

## 2. Constraint: don't touch the daemon

The existing macOS now-playing daemon (`daemon/claudemeter_daemon.py`) is
board-agnostic and already does all LRC parsing/sync on the Mac (see PLAN.md decisions
table). It must stay **unmodified** — it's shared with the ESP8266 reference device.

Confirmed from the actual daemon source (not just PLAN.md's summary, which is stale):
`push_now_playing()` sends `title, artist, pos, dur, paused, lyric, lyric2, lyric3, lt, lt2`
— a **3-line lookahead** (current / next / third) with **two promotion timestamps**, not
just current+next as PLAN.md §3 describes. This is enough to drive local slide-transition
animation without needing the full song's LRC table.

(A "send the whole song's lyrics + timestamps once, let the device pick locally" design was
considered and rejected for now: the daemon never exposes the full synced-lyrics table over
HTTP, only the resolved lookahead above, so a wrapper can't reconstruct it without either
modifying the daemon or fetching lyrics independently. Revisit only if the 3-line lookahead
proves insufficient — e.g. songs with lines spaced closer than the push interval.)

---

## 3. Architecture

```
YouTube Music tab
      │ (osascript, unchanged)
      ▼
claudemeter_daemon.py  (UNMODIFIED, device URL setting → wrapper)
      │  POST/GET /nowplaying?title=&artist=&pos=&dur=&paused=&lyric=&lyric2=&lyric3=&lt=&lt2=
      ▼
render-wrapper (new, Mac-side, Node + Puppeteer/Playwright headless Chrome)
      │  - per slot (title/artist/lyric/lyric2/lyric3): re-render to 1-bit bitmap
      │    ONLY if the string changed since last render for that slot
      │  - forwards numeric fields unchanged + bitmap blobs per slot
      ▼
ESP32-S3-RLCD-4.2 firmware
      │  - holds 3 line-bitmaps (current/next/third) + title/artist bitmaps in PSRAM
      │  - elapsed = pos + (now - pos_base_us)/1e6   (unchanged from PLAN.md, local, smooth)
      │  - at elapsed >= lt: slide-transition current→next bitmap (~200-300ms, local 70ms tick)
      │  - wide lines: local marquee scroll of the bitmap (u8g2_DrawXBM at x-offset)
      │  - progress bar / clock / rules: still plain u8g2 drawing, no shaping needed
      ▼
ST7305 panel (400×300, 1-bit, SPI)
```

---

## 4. What each smoothness goal maps to

| Goal | Mechanism | Depends on network? |
|---|---|---|
| Correct Thai shaping | Headless Chrome renders each line, wrapper-side | Only at render time, not display time |
| Smooth position/progress | `elapsed` interpolated locally from `pos_base_us`, unchanged from PLAN.md | No — local only |
| Smooth marquee (wide lines) | Local scroll offset over the bitmap, same math as PLAN.md §7a, on bitmaps instead of glyphs | No — local only |
| Seamless line transition | Local slide animation fired at `lt`/`lt2`, between bitmaps already resident | No — local only |

The 4s push cadence only has to deliver bitmaps *before* they're needed (i.e. before their
`lt`), not smoothly *during* playback — all the smoothness is reconstructed locally on the
device's own tick.

---

## 5. Wire format (wrapper → device), draft

Replaces plain-text `title/artist/lyric/lyric2/lyric3` with per-slot bitmap blobs. Numeric
fields (`pos/dur/paused/lt/lt2`) stay as-is.

Per-slot bitmap header (packed, little-endian):
```
uint16 width_px
uint16 height_px
uint32 data_len         // = ceil(width_px/8) * height_px
uint8  data[data_len]   // 1bpp, row-major, each row byte-padded (matches u8g2_DrawXBM)
```

Slots per push: `title, artist, lyric, lyric2, lyric3` (5 headers + blobs, only re-sent when
changed — device keeps the last-received bitmap per slot otherwise).

Open question: request format for "unchanged, don't resend" — either the wrapper tracks
what it last sent to *this device* and omits unchanged slots from the response body (device
keeps its cached bitmap for any slot not present in a given push), or every push includes
all 5 slots regardless (simpler, still cheap — worst case ~5-15 KB per 4s tick, trivial for
local WiFi). Lean toward "omit unchanged" since it's a small win and keeps payloads minimal.

---

## 6. Open items before implementation

- [ ] Decide wire format finalization (§5) — omit-unchanged vs always-send-all-5.
- [x] Daemon: implemented as `daemon/claudemeter_daemon.py`, based on
      `Gapyss/clawdmeter-esp8266` with ESP32 defaults. It sends `/nowplaying`
      to `http://127.0.0.1:8123` so the render wrapper can shape text before
      forwarding to `http://g4pys-company.local`.
- [x] Wrapper process: superseded by `thai_font_approach.md`; implemented as
      `tools/g4pys_render_wrapper.py` with `launchd/company.g4pys.render-wrapper.plist`.
- [x] Launchd: daemon job added as
      `launchd/company.g4pys.claudemeter-daemon.plist`. Load the render wrapper
      and daemon jobs together; the daemon has `CLAWDMETER_USAGE_SOURCE=off`
      because this firmware does not accept the upstream `/usage` payload.
- [ ] Firmware: replace `music_screen`'s `u8g2_DrawUTF8`/Tahoma-font calls (PLAN.md §7a)
      with bitmap-slot storage + `u8g2_DrawXBM` + slide/marquee offset logic.
- [ ] Drop the Tahoma font conversion work (PLAN.md §11) — no longer needed once bitmaps
      carry the text.
- [ ] Slide transition timing/easing (~200-300ms suggested, not yet tuned on hardware).
- [ ] Decide what happens if the device receives a `lt` boundary before the wrapper has
      delivered the corresponding bitmap (race on slow Chrome render or dropped push) —
      likely: hold current line until the bitmap arrives, don't blank.

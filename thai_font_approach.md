# Thai Text Rendering: native Core Text bitmap pipeline

**Status:** design draft, not yet implemented. Supersedes `render.md` §3 (architecture), §5
(wire format open question), and §6 (open items) — replaces the headless-Chrome/Node/Puppeteer
pipeline with a native macOS Core Text renderer in Python. render.md §1 (problem) and §2
(don't touch the daemon) still hold as-is.

Confirmed on hardware: the on-device u8g2 + `etl*thai_t` fallback font (PLAN.md §9's
mitigation) was flashed and tested — Thai combining-mark stacking is **not good enough**.
Real shaping is required; this isn't a preemptive rewrite.

---

## 1. Problem

Same as render.md §1: u8g2 does UTF-8 but no complex text shaping (no GSUB/GPOS). Thai
combining marks need shaping to stack correctly over base consonants. Tested on the actual
panel — not acceptable. Shaping has to happen off-device and cross the wire as a bitmap.

---

## 2. Constraint: don't touch the daemon

Unchanged from render.md §2. The existing macOS daemon (`daemon/claudemeter_daemon.py`)
stays unmodified. It already sends a 3-line lookahead —
`title, artist, pos, dur, paused, lyric, lyric2, lyric3, lt, lt2` — via
`push_now_playing()`. Its only awareness of this change is that its existing device URL
setting points at the new `g4pys.company` wrapper instead of the device directly.

---

## 3. Why Core Text, not headless Chrome

render.md originally proposed Node + Puppeteer/Playwright headless Chrome for shaping. That's
unnecessary weight: the wrapper only ever runs on the Mac, and macOS's own text stack
(Core Text / Core Graphics) already does correct Thai GSUB/GPOS shaping — it's what every
native Mac app uses. There's no CSS layout, web fonts, or emoji-color rendering need here,
just "shape a short UTF-8 string in one font → 1-bit bitmap."

Core Text over headless Chrome buys:
- No browser process, no JS engine, no DOM/CSS layout
- Render latency in the microsecond–low-millisecond range instead of Chrome's
  per-screenshot cost
- No Node/Puppeteer dependency chain to keep patched and alive under launchd
- Negligible idle memory footprint vs. Chrome's ~100s of MB even headless

**Implementation language: Python + PyObjC** (`pyobjc-framework-Quartz`/`CoreText`), not
Swift. The daemon is already Python; this keeps the whole pipeline one language/one
toolchain for what is a small, single-maintainer hobby project. PyObjC's Core Text bindings
are usable if verbose: `CTLineCreateWithAttributedString` + `CTLineDraw` into a
`CGBitmapContext`, then threshold to 1bpp.

Dependency is tracked in `tools/requirements-render-wrapper.txt`; the launchd Python must
have `pyobjc-framework-Quartz` installed.

---

## 4. Architecture

```
YouTube Music tab
      │ (osascript, unchanged)
      ▼
claudemeter_daemon.py  (UNMODIFIED, device URL setting → wrapper)
      │  POST /nowplaying?title=&artist=&pos=&dur=&paused=&lyric=&lyric2=&lyric3=&lt=&lt2=
      ▼
render-wrapper (new, Mac-side, Python + PyObjC, launchd-managed)
      │  - per slot (title/artist/lyric/lyric2/lyric3): render via Core Text/Core Graphics
      │    to a 1-bit bitmap
      │  - always renders and forwards all 5 slots, every push (stateless — see §6)
      │  - forwards numeric fields unchanged + all 5 bitmap blobs, combined in one POST
      ▼
ESP32-S3-RLCD-4.2 firmware
      │  - single /nowplaying POST: numeric fields via query string (unchanged),
      │    5 bitmap-slot structs in the raw body
      │  - holds 3 line-bitmaps (current/next/third) + title/artist bitmaps in PSRAM
      │  - elapsed = pos + (now - pos_base_us)/1e6   (unchanged from PLAN.md, local, smooth)
      │  - at elapsed >= lt: slide-transition current→next bitmap (~200-300ms, local 70ms tick)
      │  - wide lines: local marquee scroll of the bitmap, u8g2_SetClipWindow + u8g2_DrawXBM
      │    at x-offset (same clip-window idiom already used for text marquee today)
      │  - progress bar / clock / rules / eyebrow / footer: still plain u8g2 drawing —
      │    these are ASCII-only firmware string literals, not part of the 5 bitmap slots,
      │    so they need only a small ASCII u8g2 font (no Tahoma/thai font required at all)
      ▼
ST7305 panel (400×300, 1-bit, SPI)
```

---

## 5. Wire format (wrapper → device)

One combined POST per push to `/nowplaying`:
- Query string: `pos, dur, paused, lt, lt2` (unchanged shape from today's endpoint).
- Raw body: 5 concatenated bitmap-slot structures, fixed order
  `title, artist, lyric, lyric2, lyric3`, always all 5, every push.

Per-slot bitmap header (packed, little-endian):
```
uint16 width_px
uint16 height_px
uint32 data_len         // = ceil(width_px/8) * height_px
uint8  data[data_len]   // 1bpp, row-major, each row byte-padded (matches u8g2_DrawXBM)
```

Combined single POST rather than a separate bitmap endpoint: numeric fields and bitmaps are
always pushed together (see §6), so splitting them risks the two arriving out of order or
one succeeding while the other fails, for no benefit — and costs an extra TCP round-trip
every 4s, forever.

**Firmware-side validation (new, not present in today's `/nowplaying` handler):**
`width_px`/`height_px`/`data_len` are wrapper-reported, not trusted. Before allocating
PSRAM for a slot, firmware must check `data_len == ceil(width_px/8)*height_px` and clamp
against a max-dimension budget. A malformed or buggy push must not be able to over-allocate
or corrupt the heap.

---

## 6. Resolved decisions (were open in render.md §6)

- **Always send all 5 slots, unconditionally, every push.** Rejected the "omit
  unchanged slots" optimization from render.md §5: the bandwidth saved is already
  "trivial" (~5-15 KB per 4s tick on local WiFi) by render.md's own estimate, but
  omit-unchanged requires the wrapper to track per-device delivery history — and if the
  device reboots and loses its cached bitmaps while the wrapper still believes a slot was
  already delivered, that slot stays stale/blank until its text next changes. Always-send
  is stateless and self-healing. Revisit only if always-send proves too chatty in practice.
- **Wrapper lifecycle:** a second launchd plist,
  `launchd/company.g4pys.render-wrapper.plist`, same shape (`RunAtLoad` + `KeepAlive` + separate
  stdout/stderr log files).
- **New single point of failure, accepted for v1:** combining numerics + bitmaps into one
  POST (§5) means the wrapper is now a mandatory hop for *all* fields, not just Thai
  bitmaps — previously the daemon posted straight to the device. If the wrapper crashes,
  that tick's push is silently dropped (the daemon's `push_now_playing` has no retry) and
  the device freezes on stale state until the wrapper restarts. `KeepAlive=true` gives a
  sub-second restart, so worst case is one missed 4s tick — acceptable given the daemon's
  own no-retry philosophy already tolerates this class of transient failure. Wrapper should
  log each forward attempt (success/failure) to its stderr log so a frozen screen is
  diagnosable (wrapper down vs. no song playing vs. Mac asleep) without guessing.
- **Race — device receives an `lt` boundary before the corresponding bitmap has arrived**
  (slow render or dropped push): hold the current line, don't blank, until the new bitmap
  shows up.
- **Drop the Tahoma font conversion work** (old PLAN.md §11) — not needed; text no longer
  renders on-device at all except for the small ASCII-only firmware literals noted in §4.
- **3-line lookahead promotion, chained from a single timer.** The daemon's `lt` = when
  `lyric2`'s content becomes current; `lt2` = when `lyric3`'s content becomes current (two
  promotions ahead). Rather than tracking two independent timers, extend the existing
  single-promotion pattern in `music_screen.cpp` one level deeper: on `elapsed >= lt`, do
  one combined promotion — slide-transition `lyric2` → `lyric` (visible animation) and
  silently shift `lyric3` → `lyric2` (not on screen yet, no animation) — then
  `lt = lt2; lt2 = -1` (unknown until the next push refreshes it). Only one active
  promotion timer at a time. Because §6's "always send all 5" means `lyric3`'s bitmap is
  already resident before this fires, the promoted "next" line never displays without a
  bitmap. Known deferred edge case (matches render.md §2's own caveat): if `lt2` also
  elapses before the *next* push lands — lines spaced closer than the 4s push interval —
  the third slot shows blank/stale until the next push. Acceptable for v1.
- **Firmware body-reading: stream per-slot, no scratch buffer; swap under the mutex.**
  For each of the 5 slots in fixed order: `httpd_req_recv`-loop the 8-byte header first,
  validate it (§5's `data_len`/max-dimension check) *before* allocating, then
  `httpd_req_recv`-loop `data_len` bytes straight into a freshly `heap_caps_malloc`'d PSRAM
  buffer sized to that slot's actual need — no intermediate full-body buffer. Receiving
  spans multiple `recv` calls over several milliseconds on the HTTP task, while the render
  task (core 1, 70ms tick) reads the same slot buffers independently; writing new pixel
  data into a slot's *live* buffer mid-receive would tear a frame. Fix: build each slot
  fully in its own new buffer, then swap that slot's pointer+width+height into the shared
  state under the same mutex `music_screen.cpp` already uses for `NowPlaying` — a single
  atomic pointer swap, not an in-place write — and free the old buffer after the swap.

---

## 7. Design resolved, still to implement

- [x] Firmware: replace `music_screen`'s `u8g2_DrawUTF8`/Tahoma-font calls with
      bitmap-slot storage (PSRAM, pointer-swap under mutex per §6) + `u8g2_DrawXBM` +
      slide/marquee offset logic (reuse the existing `u8g2_SetClipWindow` idiom already
      used for text marquee).
- [x] Firmware: new streaming body-reading path in `http_api.cpp` per §6 — today's handler
      only reads the query string, no body-reading code exists yet.
- [x] Firmware: extend `NowPlaying`/promotion logic to the 3-line/two-timestamp chain in §6
      (`lyric3`/`lt2`) — the daemon already sends these, current firmware ignores them
      (2-line only today).
- [x] Wrapper: Python + PyObjC script — Core Text render function, HTTP forwarding logic,
      `g4pys.company` launchd plist, per-forward-attempt logging.

## 8. Still genuinely open

- [x] Slide transition timing/easing (~200-300ms suggested) — implemented at 280ms; still
      needs tuning on real hardware once the pipeline exists.

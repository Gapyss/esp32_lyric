# Implementation Handoff: MUSIC now-playing + lyrics screen (ESP32-S3-RLCD-4.2)

**Audience:** an implementing agent starting cold. Everything needed is here. Read
`../spec.md` for the board (pins, SoC, display) and this file for the feature.

> **Status:** firmware and the Mac-side `g4pys.company` bitmap wrapper are implemented.
> `thai_font_approach.md` supersedes this plan's older on-device Thai-font sections.

---

## 1. What you are building

A from-scratch **ESP-IDF** firmware for the Waveshare **ESP32-S3-RLCD-4.2** that renders a
**daemon-pushed "Now Playing + synced lyrics"** screen on the **400×300 1-bit monochrome ST7305**
LCD. It mirrors the HTTP/daemon contract of the existing macOS now-playing daemon: a Mac process reads
the current YouTube Music tab + fetches synced lyrics and pushes them over WiFi; **this device only
renders** them. **No on-device audio** (the board has a speaker/ES8311, but that is out of scope —
the reference feature is a display).

The macOS daemon (`daemon/claudemeter_daemon.py`) is **board-agnostic** and works
**unmodified**: point its existing device URL setting at the local `g4pys.company` wrapper
(`http://127.0.0.1:8123` by default). The wrapper forwards shaped bitmap payloads to the
device at `g4pys-company.local`.

---

## 2. Decisions already made (do NOT relitigate)

| # | Decision | Why |
|---|----------|-----|
| Framework | **ESP-IDF** (not Arduino) | user chose it; official audio/board demos are IDF |
| Scope | **render-only**, daemon-driven; **no on-device audio** | matches the reference feature exactly |
| Graphics lib | **u8g2** (NOT LVGL) | Waveshare ships a **proven ESP-IDF ST7305 u8g2 driver for this exact board**; it also gives UTF-8 + fonts. LVGL would force us to write a 1-bpp flush blind (the demo's LVGL path is a slow per-pixel `RLCD_SetPixel` loop we'd own). |
| Panel driver | **reuse `components/u8g2_st7305` verbatim** (already copied) | memory-in-pixel bit-packing is theirs; never hand-roll it |
| Thai + English rendering | **Mac-side Core Text bitmaps** via `tools/g4pys_render_wrapper.py` | Hardware testing showed on-device u8g2 Thai combining marks are not good enough; shaped text now crosses the wire as 1-bit bitmap slots. |
| No "dim" (1-bit panel) | current lyric = **large font**, next lyric = **small font** below; paused = **"⏸ PAUSED" text** | monochrome has no grey; the reference separated lines by color, which we can't do |
| Repaint model | **full redraw every tick** (`ClearBuffer`→draw→`SendBuffer`), one render task ~70 ms | memory-in-pixel updates clean (no flash), unlike the ESP8266+ST7789 two-phase chrome/dynamic dance in the reference — **drop that architecture entirely** |
| Marquee | **hand-rolled scroll offset** in the render task (~15 lines) | trivial on ESP32 (normal FreeRTOS task); none of the reference's ESP8266 IRAM/ISR pain |
| WiFi creds | **compile-time header** `main/wifi_secrets.h` (user edits) | minimal; do NOT build a captive portal / WiFiManager |
| Endpoints | only **`POST/GET /nowplaying`** + **`GET /usage.json`** | do NOT port the dashboard HTML, OTA, brightness, or the other screens |
| Hostname | **mDNS `g4pys-company.local`** | wrapper target for the ESP32 device |

---

## 3. The HTTP contract (from the shipped reference `.ino`, NOT the stale plan docs)

The daemon pushes (all query args, URL-encoded UTF-8; **GET or POST**):

```
/nowplaying?title=<t>&artist=<a>&pos=<sec>&dur=<sec>&paused=<0|1>&lyric=<cur>&lyric2=<next>&lt=<sec>
```

| arg | meaning |
|---|---|
| `title` | song title (UTF-8, may be Thai) |
| `artist` | artist (UTF-8) |
| `pos` | current playback position, seconds (int) |
| `dur` | total track duration, seconds (int) |
| `paused` | `1` if paused, else `0` |
| `lyric` | **current** lyric line (already resolved by the daemon; UTF-8) |
| `lyric2` | **next** lyric line, shown small below (UTF-8) |
| `lt` | playback pos (sec) at which `lyric2` should promote to current; `-1` = none |

The daemon does all LRC parsing/sync **on the Mac** — the device just displays `lyric`/`lyric2` and
(optionally, nice-to-have) promotes `lyric2`→`lyric` locally when `elapsed >= lt` so lyrics advance
smoothly between pushes.

`GET /usage.json` echoes state for `curl` debugging:
```json
{"mode":"music","title":"...","artist":"...","lyric":"...","lyric2":"...","pos":42,"dur":215,"paused":0}
```

Idle (empty title): show **`— Not Playing —`**, other regions blank.

> **GOTCHA:** `esp_http_server` does **NOT** url-decode query values (Arduino's `server.arg()` did).
> The daemon sends `urllib.parse.quote`'d UTF-8, so you MUST percent-decode each value yourself
> (`%XX` → byte, `+` → space). Write a small `url_decode(dst, src)` helper. Preserve raw bytes
> (UTF-8) — do NOT strip to ASCII.

---

## 4. Hardware / board facts (see `../spec.md`)

- **Display:** ST7305, **400×300**, 1-bit, SPI **SPI3_HOST**, pins MOSI=12 SCK=11 DC=5 CS=40 RST=41 TE=6.
  The `u8g2_st7305` component owns SPI-bus init + panel init; you just pass pins.
- **Flash 16 MB, PSRAM 8 MB octal.** `sdkconfig.defaults` (already copied) sets `esp32s3`, 16 MB QIO,
  octal PSRAM 80 MHz, custom partition table, `FREERTOS_HZ=1000`.
- Native USB-Serial/JTAG: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` may need enabling for `idf.py monitor`.

---

## 5. What is already in place (do not redo)

```
firmware/
├── PLAN.md                      ← this file
├── sdkconfig.defaults           ← copied from Waveshare 11_U8G2_Test (esp32s3, 16MB, OPI PSRAM)
├── partitions.csv               ← copied (nvs / phy_init / 8M factory app)
└── components/
    ├── u8g2/                    ← copied VERBATIM (graphics engine + all fonts incl. etl*thai)
    └── u8g2_st7305/             ← copied VERBATIM (proven ST7305 SPI driver for THIS board)
```

`components/u8g2/CMakeLists.txt` intentionally excludes `u8x8_d_*.c` and `u8g2_d_setup.c` — u8g2 is
the engine only; the display device is `u8g2_st7305`. Leave that as-is.

### Reference u8g2 bring-up (from Waveshare `main.cpp`, copy verbatim)

```c
u8g2_st7305_config_t cfg = u8g2_st7305_default_config();  // spi_host=SPI3_HOST
cfg.mosi_io = GPIO_NUM_12; cfg.sclk_io = GPIO_NUM_11;
cfg.dc_io = GPIO_NUM_5; cfg.cs_io = GPIO_NUM_40; cfg.reset_io = GPIO_NUM_41;
cfg.rotation = U8G2_R1;                       // gives a 400(w) × 300(h) landscape canvas
cfg.tile_buf_height = U8G2_ST7305_TILE_BUF_FULL;   // full-frame buffer, no tiling
ESP_ERROR_CHECK(u8g2_st7305_init(&g_lcd, &cfg));
u8g2_t *u8 = u8g2_st7305_get_u8g2(&g_lcd);
// draw loop: u8g2_ClearBuffer(u8); ...draw...; u8g2_SendBuffer(u8);
```
Do **not** experiment with rotation/coords blind — `U8G2_R1` + 400×300 is the demo's verified config.

---

## 6. Files to create

```
firmware/
├── CMakeLists.txt               ← top-level (cmake_minimum_required + include project.cmake + project())
└── main/
    ├── CMakeLists.txt           ← idf_component_register SRCS + REQUIRES + INCLUDE_DIRS "."
    ├── idf_component.yml         ← dependency: espressif/mdns  (drop the lvgl dep from the demo)
    ├── wifi_secrets.h           ← #define WIFI_SSID / WIFI_PASS   (template; user edits, add to .gitignore)
    ├── main.cpp                 ← app_main: nvs → wifi STA → mdns → display init → start httpd → render task
    ├── music_screen.h / .cpp    ← shared state (mutex) + setters + render(u8g2_t*) + marquee
    └── http_api.h / .cpp        ← esp_http_server: /nowplaying + /usage.json + url_decode helper
```

### `main/CMakeLists.txt` REQUIRES
`u8g2_st7305 esp_timer nvs_flash esp_wifi esp_event esp_netif esp_http_server mdns json`
(`json` = cJSON, optional — you can hand-build the `/usage.json` string instead and drop it.)

### `main/idf_component.yml`
```yaml
dependencies:
  idf: '>=5.0'
  espressif/mdns: '^1.2.0'
```
(The IDF Component Manager fetches `espressif/mdns` at build time.)

---

## 7. Module details

### 7a. `music_screen` — state + render

Shared state, guarded by a `portMUX`/`SemaphoreHandle_t` mutex (HTTP task writes, render task reads):
```c
struct NowPlaying {
  char  title[128], artist[128], lyric[192], lyric2[192];
  int   pos, dur, paused, lyric_at;   // as last pushed
  int64_t pos_base_us;                // esp_timer_get_time() when pos was set
};
```
- `np_set_from_args(...)` (called by HTTP handler): copy fields under mutex; set `pos_base_us = esp_timer_get_time()`.
- Local elapsed for progress/promotion: `elapsed = pos + (paused? 0 : (now - pos_base_us)/1e6)`, clamp to `[0,dur]`.
- Optional lyric promotion: if `lyric_at >= 0 && elapsed >= lyric_at` → move `lyric2`→`lyric`, blank `lyric2`.

**Render** `music_render(u8g2_t*)` — called each tick after the render task snapshots state:
- Fonts (all cover ASCII **and** Thai; use `u8g2_DrawUTF8` / `u8g2_GetUTF8Width` everywhere).
  **Primary = Tahoma** (build per §11); **fallback = bundled `etl*thai_t`** (no build step):
  - Title / current-lyric (large ~24 px): **`u8g2_font_tahoma24`**  (fallback `u8g2_font_etl24thai_t`)
  - Artist / next-lyric / times (~16 px): **`u8g2_font_tahoma16`**  (fallback `u8g2_font_etl16thai_t`)
  - Eyebrow/footer (~13–14 px): **`u8g2_font_tahoma13`**  (fallback `u8g2_font_etl14thai_t`)
  - Keep the font names in one `#define` block so swapping Tahoma⇄etl is a one-line change.
- **GOTCHA:** u8g2's `y` in `DrawStr/DrawUTF8` is the **baseline** (bottom of text), not top-left.
  Position with `baseline = top + u8g2_GetAscent(u8)`.

Suggested 400×300 layout (tune on device):
```
 y≈18   eyebrow "♪ NOW PLAYING" (left, etl14)      clock HH:MM (right, etl14)   [clock optional]
 y≈26   horizontal rule (u8g2_DrawHLine)
 y≈58   TITLE  — etl24, marquee if UTF8 width > 400
 y≈86   ARTIST — etl16, marquee if too wide
 y≈112  progress: "M:SS " + bar (DrawFrame + DrawBox fill by elapsed/dur) + " -M:SS"
 y≈130  horizontal rule
 y≈180  CURRENT lyric — etl24, centered, marquee if too wide   ← the star of the feature
 y≈220  NEXT lyric — etl16, centered (dim substitute = smaller font)
 y≈292  footer: "PAUSED" when paused, else "PLAYING"   +   "g4pys.company" (right)
```
Idle (empty title): draw only "— Not Playing —" centered in etl24.

**Marquee helper** (per scrolling line, offset lives in the render task, not shared state):
```
w = u8g2_GetUTF8Width(u8, s);
if (w <= AREA_W) { draw centered/left, reset that line's offset; }
else { draw at x = -offset, and again at x = -offset + w + GAP (wrap);
       advance offset ~1px/tick; when offset >= w+GAP, offset -= (w+GAP); }
```
Keep the scroll→static switch cheap: it's the documented Thai fallback (if scrolling stacked
tone-marks looks wrong, render Thai lines static/centered instead — a one-line branch).

### 7b. `http_api` — endpoints
- Register a single `esp_http_server` (port 80).
- `/nowplaying` (GET **and** POST): `httpd_req_get_url_query_str` → for each key
  `httpd_query_key_value` → `url_decode` → `np_set_from_args`. Respond `200 "ok"`.
  (For POST the daemon still sends args in the query string per the reference — read the URL query,
  not the body.)
- `/usage.json`: snapshot state, emit the JSON in §3. `Content-Type: application/json`.
- `url_decode(char* dst, const char* src)`: `%XX`→byte, `+`→space, else copy. UTF-8-safe (byte-wise).

### 7c. `main.cpp` — glue (order matters)
1. `nvs_flash_init()` (WiFi needs NVS).
2. `esp_netif_init()` + `esp_event_loop_create_default()` + WiFi **STA** (`WIFI_SSID/WIFI_PASS`),
   wait for `IP_EVENT_STA_GOT_IP`.
3. `mdns_init()` → `mdns_hostname_set("g4pys-company")` → `mdns_service_add(NULL,"_http","_tcp",80,NULL,0)`.
4. `u8g2_st7305_init(...)` (§5).
5. Start `http_api` server.
6. Create render task (pinned to core 1, ~4–8 KB stack): loop `ClearBuffer → snapshot state →
   music_render → SendBuffer → vTaskDelay(pdMS_TO_TICKS(70))`.

---

## 8. Acceptance checks (run on device after `idf.py build flash monitor`)

- [ ] **Bring-up = the idle screen.** First flash shows **"— Not Playing —"** on the panel. (This
      doubles as the ST7305 bring-up test — if the panel is blank, fix the driver before anything else.)
- [ ] Serial log prints the DHCP IP; `ping g4pys-company.local` resolves on the same LAN (mDNS).
- [ ] `curl 'http://g4pys-company.local/nowplaying?title=Test&artist=Me&dur=200&pos=5&lyric=hello&lyric2=world&lt=12'`
      → panel switches to the song, shows title/artist/progress/lyrics; `/usage.json` echoes fields.
- [ ] A **Latin** title/lyric wider than 400 px scrolls smoothly (marquee), no WiFi stall.
- [ ] A **Thai** title/lyric renders with correct combining marks through the `g4pys.company` wrapper.
- [ ] Percent-encoded UTF-8 (`title=%E0%B8%AA...`) decodes correctly (url_decode works).
- [ ] `paused=1` shows "⏸ PAUSED"; elapsed freezes.
- [ ] The **unmodified** daemon, pointed at `http://127.0.0.1:8123`, drives the screen from live YT Music
      through the `g4pys.company` wrapper.
- [ ] No white flash on updates (full redraw is clean on memory-in-pixel).

---

## 9. Open risk to watch

**Thai combining marks** (stacked tone/upper/lower vowels) are the one likely-imperfect thing —
u8g2 does UTF-8 but **not complex-text shaping (no GPOS)**. Mitigation is the font choice:
- **Tahoma (recommended):** its Thai combining marks have **zero advance width**, so u8g2 draws them
  on top of the preceding base consonant automatically — no manual cursor logic needed. This
  resolves *most* of the stacking risk. Rare cases (a mark over a tall/ascending consonant that
  Tahoma would nudge horizontally) stay slightly off; acceptable for v1.
- **`etl*thai_t` (fallback):** fixed-cell, marks may land in their own cell (worse stacking).

Get Latin correct first, then verify Thai on hardware. If a scrolling Thai line still looks wrong,
the cheap escape (see §7a) is rendering Thai lines **static/centered** instead of marquee. Do not
try to implement shaping.

## 10. Explicitly OUT of scope (do not build)

Dashboard HTML, OTA `/update`, brightness, WiFiManager/captive portal, the other screens
(claude/mac/desk/face/timer), on-device audio playback, any daemon changes.

---

## 11. Appendix: building Tahoma into u8g2 fonts (one-time, on the Mac)

Do this once to produce the `u8g2_font_tahoma{13,16,24}` C arrays used in §7a. u8g2 can't read TTF at
runtime — you rasterize to BDF, then convert to a u8g2 C array.

> **License:** Tahoma is proprietary (Microsoft). This is fine for building/flashing your own device.
> Do **not** commit `Tahoma.ttf` or the generated font `.c` to a public repo — add them to
> `.gitignore`. (If you need a redistributable look-alike with strong Thai, **Noto Sans Thai** /
> **Sarabun** are open-licensed and convert identically.)

**Tools (macOS):**
```sh
brew install fontforge          # provides otf2bdf-style rasterization; or `brew install otf2bdf`
# u8g2 bdfconv: build from the u8g2 source tree
git clone --depth 1 https://github.com/olikraus/u8g2 /tmp/u8g2src
make -C /tmp/u8g2src/tools/font/bdfconv        # produces the `bdfconv` binary
```

**Convert (repeat per size — 13, 16, 24):**
```sh
# 1) TTF -> BDF at a pixel size (otf2bdf; -p = pixel size)
otf2bdf -p 24 -o tahoma24.bdf /path/to/Tahoma.ttf

# 2) BDF -> u8g2 C array. -f 1 = u8g2 format; -m = Unicode codepoint map:
#    ASCII 32-126 + Thai block U+0E00-U+0E7F (3584-3711).
/tmp/u8g2src/tools/font/bdfconv/bdfconv -v -f 1 \
    -m '32-126,3584-3711' \
    -n u8g2_font_tahoma24 \
    -o tahoma24.c  tahoma24.bdf
```
`bdfconv` preserves each glyph's `DWIDTH` (advance) and `BBX` (offset) from the BDF — that's what
keeps Thai marks zero-advance and correctly offset, so u8g2 stacks them on the base.

**Wire into the build:**
```
firmware/components/fonts_tahoma/
├── CMakeLists.txt        # idf_component_register(SRCS tahoma13.c tahoma16.c tahoma24.c INCLUDE_DIRS ".")
├── fonts_tahoma.h        # extern const uint8_t u8g2_font_tahoma13[]; ...16[]; ...24[];
├── tahoma13.c  tahoma16.c  tahoma24.c
```
Add `fonts_tahoma` to `main/CMakeLists.txt` REQUIRES, `#include "fonts_tahoma.h"`, and point the §7a
font `#define`s at the tahoma symbols. If any glyph is missing on-device, it just doesn't draw —
check the `-m` range covered it.

**Sanity check before flashing:** `bdfconv -v` prints the glyph count and a preview per glyph; confirm
Thai consonants (ก U+0E01) and a tone mark (่ U+0E48) are present and the mark shows above the cell.

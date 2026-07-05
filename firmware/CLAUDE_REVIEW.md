# Claude Review Notes

> **SUPERSEDED (bitmap pipeline).** The notes below review the earlier
> on-device u8g2/Tahoma text implementation (`draw_utf8_scrolled`, glyph
> x-offset guards, Tahoma font swap). That path no longer exists: `music_screen`
> now stores per-slot 1-bpp bitmaps shaped Mac-side by Core Text
> (`thai_font_approach.md`) and draws them with `u8g2_DrawXBM` /
> `draw_bitmap_pixels`. Treat everything under "Scrutiny findings" about glyph
> offsets and font swaps as historical.


Implemented the firmware scaffold from `PLAN.md`, then scrutinized it end-to-end
against the plan and the u8g2 source.

## Added

- ESP-IDF project files: `CMakeLists.txt`, `main/CMakeLists.txt`, `main/idf_component.yml`.
- ST7305 display init and render task startup happen before blocking WiFi connect, so first flash can show the idle screen even when WiFi credentials are wrong.
- WiFi STA startup using `main/wifi_secrets.h`, mDNS hostname `g4pys-company.local`, and HTTP API startup in `main/main.cpp`.
- `/nowplaying` GET/POST and `/usage.json` in `main/http_api.cpp`, including byte-wise percent decoding for URL-encoded UTF-8. Empty probes without a `title` query arg return `ok` without mutating display state.
- Shared now-playing state, elapsed-time calculation, render-only local `lyric2` promotion, progress bar, idle screen, and marquee rendering in `main/music_screen.cpp`.
- `.gitignore` entries for local build products, WiFi secrets, managed components, and optional private Tahoma font component.

## Known deviations from `PLAN.md`

- **Media/symbol glyphs are plain ASCII, not the spec's symbols.** PLAN §8/§7a call for
  `♪ NOW PLAYING`, `⏸ PAUSED`, `▶`, and `— Not Playing —` (em dashes). The code ships
  `NOW PLAYING`, `PAUSED` / `PLAYING`, and `-- Not Playing --`
  (`music_screen.cpp:279,284,304-306`). Reason: the bundled `u8g2_font_etl*thai_t` fonts are
  TIS-620 cell bitmaps and do **not** carry those Unicode codepoints, so drawing them would
  render nothing/boxes. This is cosmetic — the paused state is still shown as text — but it is a
  literal-spec gap and a latent TODO: when you swap to Tahoma (§11), which *does* carry the
  em-dash and media glyphs, these hardcoded ASCII strings will **not** auto-upgrade. Update the
  string literals at the same time as the font `#define`s.

## Notes

- The implementation currently uses bundled `u8g2_font_etl{14,16,24}thai_t` fonts so it builds without committing private Tahoma-generated font files. Swap the `FONT_LARGE/MEDIUM/SMALL` macros in `music_screen.cpp` after adding a local `fonts_tahoma` component (and see the marquee caveat below).
- `usage.json` is hand-built, so `json`/cJSON is intentionally not in `main/CMakeLists.txt`. Its `pos` field reports live locally-advanced elapsed seconds (`snapshot.elapsed`), not just the last pushed `pos`.
- `GET /usage.json` snapshots with `allow_promote=false`; only the render task (`allow_promote=true`) advances the on-screen lyric.
- WiFi secrets: `main/wifi_secrets.example.h` is the committed template; `main/wifi_secrets.h` holds the real credentials and is gitignored. Edit `wifi_secrets.h` before flashing.

## Scrutiny findings

- **Marquee glyph-visibility check is correct for the etl fonts, but has a latent Tahoma bug.**
  `draw_utf8_scrolled` (`music_screen.cpp:166-185`) gates each glyph on
  `glyph_right > area_x && glyph_left < area_end && glyph_left >= 0`, where `glyph_left`/`glyph_right`
  come from `u8->glyph_x_offset` and `u8->font_decode.glyph_width`. Verified those fields are
  populated by `u8g2_GetGlyphWidth` before they are read (`u8g2_font.c:915` sets `glyph_x_offset`,
  `:590` sets `glyph_width` via `u8g2_font_setup_decode`). The `glyph_left >= 0` guard exists to stop
  a negative x from wrapping under `u8g2_uint_t` (uint16). But the coordinate actually passed to
  `u8g2_DrawGlyph` is `pen_x`, not `glyph_left` — the two diverge exactly when `glyph_x_offset`
  is nonzero. For the fixed-cell etl fonts `glyph_x_offset ≈ 0`, so `glyph_left ≈ pen_x` and the guard
  is effectively `pen_x >= 0`; harmless today. Under **proportional Tahoma**, Thai combining marks
  carry a *negative* x-offset (to stack left of the base). A mark sitting near the left clip edge would
  have a valid `pen_x` but `glyph_left < 0`, so this guard drops it even though u8g2 would clip it fine.
  Re-check Thai rendering at the left edge after the font swap; the fix is to guard on `pen_x >= 0`.
- **No blockers found.** State is mutex-guarded (`music_screen.cpp`), the `/usage.json` string
  builder caps every field via bounded `json_escape`/`snprintf` (worst-case escaped length stays under
  the 1536-byte body buffer), percent-decoding preserves raw UTF-8 bytes, and `has_title` correctly
  prevents empty probes from clobbering state.

## Verification

- Prior build (not re-run in this review — ESP-IDF is not sourced in this shell): `idf.py build` passed with ESP-IDF v5.5.1; artifact `build/g4pys_company_rlcd.bin` is present (~874 KB). The 8 MB app partition had about 89% free.
- A stale binary from the earlier project name may remain in `build/`; harmless, gets cleaned by `idf.py fullclean`.
- Plain `idf.py build` is not available until the ESP-IDF export script is sourced.

## Verdict

**fix-then-ship (doc/polish level, not code):** the firmware is correct and matches the plan's
behavior; before calling it done, decide whether the ASCII-vs-symbol substitution is acceptable for
v1 and remember the marquee `pen_x` guard when swapping in Tahoma. Neither blocks flashing.

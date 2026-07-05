# Long Lyric Line Review Handoff

This document covers the shrink-then-wrap change for the active lyric line in
`daemon/lyrics_display_daemon.py`.

## What Changed

- Added current-lyric layout constants for the 400x300 frame:
  - base size `30`
  - wrap size `26`
  - minimum size `14`
  - active lyric box width `DISPLAY_WIDTH - 28`
  - vertical band from y `130` to y `204`
- Added the pure `fit_lyric_layout(...)` helper.
  - It accepts an injected line breaker: `break_fn(text, size, width) -> list[str]`.
  - It returns font size, 1-2 row strings, row baselines, bottom-row baseline, highlight y, and `hide_third`.
  - Unit tests use a fake breaker and cover control flow only.
- Updated `CoreTextFrameRenderer.render(...)` so only the active/current lyric slot uses the new layout.
  - Short current lines still render as one row at size `30`, baseline y `178`.
  - Mildly long lines shrink down toward size `26` before wrapping.
  - Longer lines wrap to at most two rows at size `26`.
  - Pathological lines shrink toward size `14`; if still overfull, the second row keeps remaining text and CoreText clipping remains the safety net.
- Added `CoreTextFrameRenderer._break_text(...)` backed by `CTTypesetterSuggestLineBreak`.
  - This keeps Thai dictionary breaking, CJK behavior, and Latin word-aware breaking in CoreText.
  - It uses `NSString` indexing so CoreText offsets and substrings use the same UTF-16 coordinate system.
- Updated the karaoke highlight bar.
  - One highlight bar is still drawn.
  - For one row, it remains below the original baseline.
  - For two rows, it moves below the bottom row.
- Hidden the `third` lyric line while the current lyric wraps.
  - `next` remains fixed at baseline y `224`.
  - `third` is cosmetic decrowding only; its slot is not reused for the wrapped current lyric.

## Review Focus

- Check that only the current lyric slot changed. Title, artist, next line, third line, progress, and footer should keep the existing clipped `CTLine` behavior.
- Check the geometry: wrapped rows should stay visually centered between the header rule at y `130` and the fixed next-line top area around y `204`.
- Check that the single-line shrink floor and wrap size are both `26`; lowering the single-line floor would prevent wrapping from firing soon enough.
- Check `_break_text(...)` on macOS with PyObjC installed. The unit tests do not validate Thai/CJK break-point correctness.
- Check that scheduled frames get the same behavior through the existing `renderer.render(future_state)` path without scheduler changes.
- Check that hiding `third` is tied only to actual two-row current lyric layouts.

## Fix Applied (post-review)

`scrutinize` found that the karaoke highlight bar overflowed the lyric band
for two-row wraps at font sizes ~21-26 (`highlight_y` reached 205-208,
inside the `next` line's own glyph area at baseline y `224`), because
`highlight_y` was computed as `bottom_row_baseline_y + highlight_gap_y`
with no ceiling. Fixed in `_lyric_layout(...)` by clamping `highlight_y` to
`band_bottom_y` (`sizes.band_top_y + box_h`, i.e. y `204`). Verified via a
sweep over two-row wrap sizes 14-26: `highlight_y` no longer exceeds `204`.
Updated the corresponding assertion in
`tests/test_lyrics_display_daemon.py::test_long_line_wraps_to_two_rows_at_wrap_size`
from `208` to `204`.

## Local Verification Run

These checks passed:

```sh
python3 -m unittest tests/test_lyrics_display_daemon.py
python3 -X pycache_prefix=/tmp/pycache-esp32 -m compileall daemon tests
```

The first `compileall` attempt without `-X pycache_prefix` failed because the sandbox could not write to the macOS user Python cache under `~/Library/Caches`. The `/tmp` pycache prefix avoids that sandbox-only issue.

## Not Verified Here

CoreText visual rendering was not verified in this environment because the active Python interpreter does not have PyObjC/CoreText installed:

```text
ModuleNotFoundError: No module named 'CoreText'
```

Manual verification still needed on a Mac Python environment with PyObjC:

- Render a long Thai lyric line and confirm it wraps using sensible dictionary break points.
- Render a long Latin lyric line and confirm it shrinks, then wraps at word-aware breaks.
- Confirm the highlight bar sits below the bottom row when wrapped.
- Confirm `next` stays fixed and `third` disappears only while wrapped.

## Files Touched

- `daemon/lyrics_display_daemon.py`
- `tests/test_lyrics_display_daemon.py`

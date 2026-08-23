# Radar Screen — Design

A rain-radar scope for the ESP32-S3-RLCD-4.2. Shows live precipitation around a
fixed home location, plus rain probability at +3h and +7h.

Status: **implemented** in `firmware/main/radar_screen.cpp`. Decisions below were
settled in a design interview and validated against live API data on 2026-08-23.
Where the build diverged from this document, the divergence is recorded under
"Built" at the end.

## Product

- **Scope (300x300, left):** current radar returns around home, dark-scope style,
  with a continuously sweeping beam.
- **Panel (100x300, right):** two numbers — `+3H` and `+7H` rain probability —
  over a `DATA HH:MM` stamp for the capture time of the frame on screen.

Everything runs on the board. No Mac, no daemon, no `lyrics_display_daemon.py`
involvement. The screen keeps working when the Mac is asleep.

## Verified facts

Measured live on 2026-08-23, not assumed.

### Location and tile constants

Home is `15.3919001, 99.8456348` (inland central Thailand). Because the location
is fixed, every tile constant below is a **compile-time constant**.

| | Value |
|---|---|
| RainViewer tile | `/512/7/99/58/` (size 512, zoom 7, x 99, y 58) |
| Home pixel inside tile | `(256, 236)` — the `tools/` script computes this; do not hardcode |
| Scope crop | x `105..405`, y `86..386` (300x300) |
| Scale | 0.59 km/px |
| Scope radius | 148 px = **88 km** |
| Range rings | 37 / 74 / 111 / 148 px = 22 / 44 / 66 / 88 km |

A single 512 px tile covers the whole scope with ~86 px of margin. **One fetch,
one PNG, one decode.** 256 px tiles would need 9 tiles for the same coverage.

### RainViewer

- Index: `https://api.rainviewer.com/public/weather-maps.json` — keyless.
- Frame `path` is a **hash**, not derivable from a timestamp. The index must be
  fetched first, every time. A full refresh is therefore **3 requests**:
  RainViewer index + 1 tile + Open-Meteo.
- 13 past frames at 10-minute intervals (120 min of history).
- **`nowcast` array is empty** — zero future frames. The radar shows now and the
  past only. All forward-looking information comes from Open-Meteo.
- **The colour-scheme parameter is ignored.** Schemes 0/2/4/8 return byte-identical
  palettes. The palette is therefore stable and its LUT can be baked.

### RainViewer pixel encoding

Two interleaved ramps, plus a clutter band:

| Alpha | Colours | Meaning |
|---|---|---|
| 130-190 | 7 beige tones | clutter / haze — **discard** |
| 255 | 40 colours | real rain |

Within alpha 255, intensity is encoded in **colour**, not alpha:

- **cyan -> dark blue** — light to moderate. Ordered by the *green* channel
  (cyan has g~209-221; dark blue has g~71-127). Luminance is **not** a valid
  ordering: the warm ramp interleaves with it.
- **yellow -> orange -> red** — heavy.

`rgb(108,209,235)` — the second-lightest cyan — was **32% of all rain pixels**
on its own. The two lightest cyans together were ~38%. This band is the reason
naive rendering turns into a grey wash.

### Open-Meteo

```
https://api.open-meteo.com/v1/forecast
  ?latitude=15.3919&longitude=99.8456
  &hourly=precipitation_probability
  &current=precipitation
  &forecast_hours=8
  &timezone=Asia%2FBangkok
```

- Keyless. **479 bytes.**
- `forecast_hours=8` anchors the array at the **current hour**, so
  `precipitation_probability[3]` and `[7]` are literally +3h and +7h. No date
  parsing on the board.
- Caveat: index 0 is the current *hour*, so at 16:30 the "+3h" value is ~2.5h out.

## Decisions

| # | Decision | Notes |
|---|---|---|
| 1 | Real radar imagery over a real map, not a stylised dial | |
| 2 | All board-side | no Mac/daemon dependency |
| 3 | Location baked at build time | fixed home; tile indices are constants |
| 4 | **Dark scope** — black field, white marks | chosen over the light "Tend" theme |
| 5 | Both percentages from Open-Meteo | single source, so the two numbers can't contradict |
| 6 | **Continuous** sweep, 60 s/revolution | beam doubles as a minute hand |
| 7 | 70 ms render cadence (`RENDER_PERIOD_ANIMATED_MS`) | matches `sand_screen` |
| 8 | Layout: 300 px scope + 100 px panel | numbers never overlap rain |
| 9 | **Current frame only** — no echo trail | reversed after the proof render |
| 10 | Panel shows **percentages only** | no condition text |
| 11 | Landmarks instead of a basemap | 3-5 labelled dots |
| 12 | 8th stop in the mode cycle | |
| 13 | **ESP32 only** — not `esp8266/` | 240x240, no PSRAM for a 512 px decode |
| 14 | Fetch **only while displayed** | zero cost on other screens |
| 15 | **Centre marker filled when raining at home** | driven by Open-Meteo `current.precipitation`, not radar |
| 16 | **Light rain drawn, at 2/16** | reverses the MODERATE+ floor; the wash came from the density, not the band |

### Why 60 s/revolution at 70 ms

Beam-tip movement is `radius x angle`. At r=148, a 3 deg step jumps the tip
~7 px — visible stepping at the rim. Pixel-smooth motion needs ~0.4 deg/step,
which at 60 s/rev is ~68 ms/frame. That is `sand_screen`'s existing cadence.

Cost note: the perf commit (`96274f0`) skips the SPI push when the frame is
unchanged. A spinning beam changes every frame, so **that optimisation never
fires on this screen**. Load is identical to `sand_screen`, which is already
accepted. It applies only while the screen is displayed.

## Rendering

Dark theme is *less* code than the existing light one: a cleared u8g2 buffer is
already all-0 (black), so it is `ClearBuffer` + `SetDrawColor(1)` with no fill
pass. `clock_screen.cpp:68` notes each screen carries its own theme helper, so a
local `begin_scope_theme()` matches convention.

### Intensity tiers

```
if (alpha != 255)          -> discard (clutter)
blue ramp (b > g && b >= r):
    g >= 200               -> tier 0   (lightest cyan — light rain)
    g >= 160               -> tier 1
    else                   -> tier 2
warm (r >= 200)            -> tier 3
```

Threshold is **LIGHT+** — everything above the clutter band is drawn. This
**reverses the original MODERATE+ floor** (see decision #16): light and
moderate rain were both invisible, and light rain is most of what falls here.
Measured scope coverage: all returns 43%, moderate+ 21%.

### Fill

4x4 Bayer ordered dither, densities `[2, 4, 8, 16]/16` for tiers 0-3.

Tier 0 is ~38% of all rain pixels on its own, so it is what turned the scope
into a grey wash at the original `4/16`. At `2/16` — one pixel in eight — it
reads as a sparse stipple that is clearly separable from tier 1 above it, and
each tier from there roughly doubles. Measured on the 2026-08-23 tile:
tier 0 covers 15.9% of the scope for 2.0% ink, tier 1 3.6% for 0.9%, and total
scope ink goes 4% -> 7%. That tile carried **4 heavy pixels**, so it does not
test a heavy day; on the doc's recorded worst case the same arithmetic puts ink
near 19%, against the 16% that was already accepted. `RADAR_TIER_FLOOR` is kept
as the one-line knob for going back to moderate-and-above if that proves busy.

The Bayer matrix must be **anchored to screen space**, not regenerated per
frame or per-pixel randomised — a rotating beam over per-pixel noise boils.

### Beam

- Angle is derived from the **wall clock**, not a free-running counter:
  `angle = (tv_sec % 60) / 60 * 360`. A free-running loop drifts and is not a
  minute hand. NTP is already set up in `time_sync_task` (`TZ=ICT-7`).
- 40 deg trailing wedge, max density 7/16, linear fade to the tail.
- 1 px bright leading edge.
- 90 deg at full density was tested and **obliterated a quarter of the scope**.

### Furniture

Range rings at 37/74/111/148; full-width crosshair; bearing ticks every 15 deg
(8 px at 45 deg multiples, else 4 px); centre marker as a small open circle with
a 14 px cross. The centre circle is **filled** when Open-Meteo reports
`current.precipitation > 0` and **open** when it does not — see the home-pixel
section below for why this is not driven by the radar.

### Panel

`+3H` / `+7H` eyebrows with large numerals beneath, divider rule between.
Verified legible at 100 px width.

### Measured result

Scope ink coverage **16%** on a heavy-monsoon frame — down from 43% before
thresholding. The threshold comparison that settled this was run against live
data during the design interview; `tools/radar_preview.py` is the surviving
form of it, and holds the verified tier classifier, Bayer densities, tile math,
landmark projection and 40-entry LUT alongside the shipped layout.

## Radar and Open-Meteo disagree at the home pixel

Measured 2026-08-23 16:30: Open-Meteo reported `precipitation = 0.1`,
`weather_code = 51` (drizzle) at home, while the radar tile showed **0/29 wet
pixels within 3 px of the home pixel at every threshold, including ALL
returns**. Light rain first appears ~8 px out (~5 km).

This is not a thresholding artefact — drizzle at 0.1 mm is below what the radar
resolves. Loosening the threshold does not fix it and would restore the wash.

Because the panel shows percentages only, nothing on screen would otherwise
report current rain at home. **Agreed fix: the centre marker is filled when
Open-Meteo's `current.precipitation > 0`, and open when it is not.** This
costs no panel space, adds no text, and answers at the exact pixel the eye
checks first. It requires adding `&current=precipitation` to the Open-Meteo
query (still keyless, still well under 1 KB).

## Staleness

If the newest frame is older than ~25 min (Wi-Fi down, API down), the scope is
showing rain that has moved on.

- Range label switches to frame age (`-34M`). The `DATA` stamp keeps showing
  the capture time either way.
- **The beam stops.** A frozen sweep reads as "not live" with no text needed.

## Integration points

- `app_config.h` — add `APP_MODE_RADAR = 7`
- `app_mode.cpp` — **three** places: validation in `app_mode_set`, the toggle
  chain, and `app_mode_name`. Note `APP_MODE_APOD` currently falls through to
  `default` rather than having its own `case`, so the chain needs rewiring, not
  just an append.
- `main.cpp` — refresh dispatch (~199-219), render dispatch (~417-431), and
  `render_period_ms()` animated case
- `CMakeLists.txt` — add `radar_screen.cpp` to `SRCS`
- `http_api.cpp` — mode parsing
- `FEATURE_INDEX.md` — repo convention is that every feature is indexed

## Risks / open

- ~~`pngle` alpha handling~~ — **checked, not a risk.** `png_draw` in
  `comic_screen.cpp:275` already receives `const uint8_t rgba[4]` and already
  reads `rgba[3]` (it blends toward white for opaque comics). It also already
  declares **the identical 4x4 Bayer matrix** this design specifies. The radar
  screen needs its own draw callback (`rgba[3] != 255 -> discard`, then classify
  colour into tiers) but no new decoder plumbing.
- **No direction information.** With the echo trail dropped, nothing on screen
  says whether rain is inbound or departing; the percentages carry that entirely.
- **Scope shows ~2 h of approach**, not 3. At 88 km radius and 30-50 km/h storm
  speeds. Do not label the scope as showing +3h.
- ~~Landmark coordinates for the 3-5 towns still need picking and projecting.~~
  **Done.** `tools/radar_landmarks.py` resolves candidate town names through
  Open-Meteo's keyless geocoding API, projects them with the same Mercator math
  as the firmware, and keeps one per bearing quadrant. Sorting by population
  alone piled every label into the south-east; the quadrant rule is what gives
  the scope something to read against in every direction. The script also
  double-checks the projection: "Nong Chang" lands 0.3 km from home, which is
  the district home actually sits in.

## Built

Implemented 2026-08-23. Five places where the build departs from the design
above, each measured rather than assumed:

1. **Crop is x 106..406, not 105..405.** The window is derived from the
   projected home pixel at startup instead of being pasted in, which puts home
   exactly on the scope centre. The 1 px shift is rounding the table did by
   hand.

2. **The tier classifier's warm branch is unconditional.** The specified
   `r >= 230 && g >= 100` / `r >= 200` pair left `rgb(168,0,0)` -- a deep red,
   the strongest return on the tile -- falling through to tier 2. Anything that
   is not the blue ramp *is* the warm ramp, so it now returns tier 3 outright.
   Measured against a live frame the classifier otherwise reproduces this
   document exactly: tier 2 at 20.6% and all returns at 42.5%, against the 21%
   and 43% recorded above.

3. **The beam iterates rays, not pixels.** The proof script's per-pixel `atan2`
   over 90,000 pixels does not fit in a 70 ms frame on this chip. Iterating 100
   rays at 0.4 deg is the same picture for ~14,800 integer steps. The 0.4 deg
   figure derived above for motion smoothness is also exactly what keeps the
   radial fill gapless at the rim.

4. **Sub-second beam angle.** `(tv_sec % 60) / 60 * 360` alone steps 6 deg per
   second, which is a 15 px jump at the rim. The angle carries `tv_usec` as
   well, so it stays clock-locked *and* smooth.

5. **Landmark labels knock out the field behind them.** A 5x7 label laid
   straight over dithered returns is unreadable, and heavy rain is exactly when
   the landmarks matter. The knockout clips the range rings where they pass
   behind a label, which is the cheaper of the two losses.

6. **The stale beam parks at 12 o'clock.** The design says the beam stops, and
   it does. Deriving the parked angle from `frame_time` would have been the
   obvious way to do it, but RainViewer stamps every frame on a 10-minute
   boundary, so `frame_time % 60` is always 0 and the two are the same picture.
   The constant is the honest spelling.

7. **The intensity floor moved to LIGHT+.** See decision #16: the wash came
   from filling the light band at `4/16`, not from drawing it at all. At `2/16`
   it is a stipple, and light rain -- most of what falls here -- is on the
   scope. Scope ink went 4% -> 7% on the 2026-08-23 tile -- which carried four
   heavy pixels, so it does not test a heavy day. The worst-case figure of ~19%
   quoted under Fill is arithmetic on this document's coverage numbers, not a
   measurement.

8. **The panel carries a `DATA HH:MM` stamp.** Decision #10 said percentages
   only, and the range line reports staleness only once a frame is 25 min old.
   In between, nothing said *when* the imagery was captured. The stamp is the
   RainViewer frame time in local time, so it *should* read correctly even
   before SNTP has set the board clock -- the case where the beam is parked and
   the age line says nothing. Not verified: a frame only exists after a fetch,
   and SNTP has normally run by then, so the preview cannot reach that path.
   Labelled `DATA` so it is not read as a clock.

Verification without hardware: `tools/radar_preview.py` renders the shipped
layout from live data, mirroring the projection, classifier, dither, beam and
panel geometry, and reports label fit using the real u8g2 glyph advances. `--stale N` forces
the frame age so the stopped beam and the `-34M` label can be looked at. Beam
load measured there is 2.2% of scope ink, filling ~20% of its own wedge.

Not yet verified on hardware: nothing in this feature has been run on the
board. Decode timing, the 70 ms frame budget with a live PNG decode in flight,
and how the ST7305 actually holds the dither are all still open.

---

# Revision 2 — the sweep carries motion

Settled in a design interview on 2026-08-23, after the build above shipped.
Nothing in this revision is implemented yet.

## Why

The sweep's only stated job was decision #6: "the beam doubles as a minute
hand." **It does not.** `radar_render_current` drives the angle from
`now_tv.tv_sec % 60` (`radar_screen.cpp:894`) — one revolution per *minute*,
which makes it a second hand. Nobody reads a scope for the current second.

That false premise was load-bearing. 60 s/rev forced 0.4 deg/step, which forced
the 70 ms cadence, which is why the doc admits the `96274f0` frame-skip
optimisation "never fires on this screen." The sweep was spending the power
budget of a permanently-changing frame to convey one bit — live vs stale —
that the `DATA` stamp and `-34M` footer already print as text.

Meanwhile the doc's own Risks section names the gap: *"No direction
information. With the echo trail dropped, nothing on screen says whether rain
is inbound or departing."* The beam is the only channel on the scope not
carrying data, and direction is the only datum the design admits is missing.
Revision 2 connects those two facts.

## Decisions

| # | Decision | Notes |
|---|---|---|
| 17 | The beam carries **storm motion** | the gap the doc already admits to |
| 18 | **Tell, not show** — compute a vector, don't animate history | Tell is a strict *prefix* of Show: 2 frames vs 13, same plumbing |
| 19 | Vector from **radar cross-correlation**, not Open-Meteo wind | 10 m wind under a cell is often outflow — near-reversed on the days that matter |
| 20 | ETA measured by **upstream corridor**, half-width ~15 px (~9 km) | walk back along `-v` from home; first qualifying cell gives `d / \|v\|` |
| 21 | **`RADAR_ETA_TIER_FLOOR` is independent of `RADAR_TIER_FLOOR`**, default tier 2 | tier 0 is 38% of rain pixels and 15.9% of scope: an ETA keyed to it is pinned near zero all monsoon |
| 22 | **Precedence rule**, not a source change | `current.precipitation > 0` outranks radar: ETA prints `NOW`, never `CLEAR` |
| 23 | Bearing drawn as **thin radial line + rim chevron** | a static 40 deg wedge would permanently occlude the upstream sector — the one that matters |
| 24 | **The sweep stays**, alongside the bearing marker | reaffirmed after #25: the original reason (peripheral liveness) is void at 1.55 px/s — it stays because it is the only thing on screen saying **when the picture gets replaced** |
| 25 | Sweep is **10 min/rev** = one RainViewer frame interval | the sweep becomes a countdown to next data; render drops to `RENDER_PERIOD_STATIC_MS` |
| 26 | Correlation confidence: **three gates, all must pass** | signal / match / **distinct peak** — the margin gate is the only one that catches flat surfaces |
| 27 | Verification is **build and flash**, no offline probe first | accepted risk, see Unverified |

### Supersedes

- **#5** ("both percentages from Open-Meteo — single source, so the two numbers
  can't contradict") — the screen is now genuinely two-source. Replaced by an
  ownership boundary: **radar owns now and soon, Open-Meteo owns hours out**,
  with #22 as the precedence rule at the seam.
- **#6** (60 s/rev, "minute hand") — replaced by #25. The justification was
  never true.
- **#7** (70 ms `RENDER_PERIOD_ANIMATED_MS`) — at 10 min/rev the tip moves
  0.042 deg per 70 ms frame, which is invisible. At 1000 ms it advances
  0.6 deg/step, ~1.55 px at the rim. **14x fewer SPI pushes.**
- **The stale-park mechanism** (Built note #6). See "Liveness" below: at
  1.55 px/s a turning beam and a parked beam are the same picture.
- **#10** (panel shows percentages only) — the ETA is a duration, not a
  percentage.

`#15` (centre marker from Open-Meteo `current.precipitation`) **stands.** The
doc measured radar showing 0/29 wet pixels at home while Open-Meteo reported
0.1 mm drizzle; deleting the more sensitive instrument to win an architectural
argument would make the screen less correct on the one case that was measured.

## Motion vector

### The decoder must emit a second output

`radar_png_draw` applies the Bayer dither **inside** the decode callback
(`radar_screen.cpp:356-358`) and writes straight into the 1bpp scope. There is
no undithered intermediate. Correlating two dithered buffers would measure the
Bayer phase as much as the rain, because the matrix is screen-anchored and a
storm shifted 11 px re-stipples against a different phase.

So the callback gains an **undithered mask** — one more `|=` in the same loop,
one more 11,400-byte PSRAM buffer. The previous distinct frame's mask is
retained for correlation.

**The mask floor is `RADAR_CORR_TIER_FLOOR`, a third constant, and it is not
`RADAR_ETA_TIER_FLOOR`.** The two thresholds have opposite requirements:
correlation wants a well-populated, stable feature map, while the arrival test
wants a high bar so drizzle does not trigger it. Building the mask at tier 2
would leave it nearly empty on a light-rain day, gate 1 would fail, and **no
vector would be drawn for rain whose motion was perfectly measurable at tier
0/1**. That is decision #21's reasoning applied one level down. Correlate at
the display floor (or tier 1), threshold the corridor at tier 2.

### Steady state costs zero extra fetches

The partner frame is the one already in hand from the last refresh. Key the
pair on `frame_time` and use the actual `Δframe_time` to convert pixel
displacement into km/h. Only a cold start lacks a partner, and the honest
response there is no vector for one cycle — not a second tile fetch.

### Prerequisite: the frame_time short-circuit

`REFRESH_INTERVAL_US` is 5 minutes (`radar_screen.cpp:108`); RainViewer
publishes every 10. `refresh_scope` parses `frame_time` at line 481 then
unconditionally fetches and decodes the tile at line 493 — **no check against
the frame already held.** Roughly every other refresh re-downloads a PNG it
already has and re-decodes it into a byte-identical bitmap.

A guard after line 481 (`if (frame_time == g_radar.frame_time) return
ESP_OK;`) halves tile traffic and decodes on this screen. It is also
**load-bearing for correlation**: it is what guarantees a frame is never
correlated against itself, which would yield a confident `|v| = 0`.

### Correlate over an inner disc

`radar_png_draw:352` rejects everything outside `SCOPE_RADIUS`, so the mask is
a **disc**. Shift a disc against a disc and the overlap area shrinks with
`|offset|` — raw AND-popcount is therefore **maximised at dx=dy=0 by geometry
alone**, independent of where the rain went. Naive correlation reports
"stationary" on days when rain is racing across the scope.

Fix: correlate only over an inner disc of radius `SCOPE_RADIUS - MAX_SHIFT`
(148 - 20 = **128 px**), so every candidate offset sees an identical,
fully-populated region. Normalise the score by the set-bit count in the
reference window, so it is a *fraction matched* and comparable between a
drizzle day and a monsoon one.

Search range: at 40 km/h over a 10-minute gap, rain moves 6.7 km ≈ **11 px** at
0.59 km/px, so ±20 px covers up to ~72 km/h. Coarse-to-fine (step 4, then
refine ±3) is ~150 offsets instead of 1,681 — well under 10 ms, once per ten
minutes, on the refresh path and never in a render frame.

### The three gates

All must pass or **nothing is drawn**:

1. **Enough signal** — the reference window holds at least *N* wet cells.
2. **Good match** — best score above an absolute fraction floor.
3. **Distinct peak** — best score beats the best *non-adjacent* candidate by a
   margin.

Gate 3 is the one that matters. Widespread stratiform rain — common here —
passes gates 1 and 2 with room to spare and still has no recoverable
displacement, because every offset matches about equally well. Only a distinct
winner separates real motion from a coin flip.

Also rejected: `|v|` below ~10 km/h (a stalled cell has no arrival time), and a
corridor empty to the rim (that is `CLEAR`, which is a real answer).

### The ETA must decay between frames

`d / |v|` is computed once per distinct frame, and frames are **10 minutes
apart**. Displayed unchanged, `RAIN 25M` would still read `25M` when it means
`15M` — a headline number wrong by up to ten minutes, on the one value whose
entire worth is its precision.

Display `eta_at_frame_time - (now - frame_time)`, floored to `NOW`. The panel
then changes once a minute, which at 1000 ms cadence costs nothing: the
`memcmp` skip at `main.cpp:444` absorbs the other 59 renders.

### The wedge and the number vanish together

Never a bearing without an ETA, never an ETA without a bearing. A half-answer
on a glanceable screen is read as a whole one. When no vector survives the
gates, the sweep keeps turning — so the scope still reads as live and current,
just declining to guess.

A screen that says nothing is trustworthy. A screen that says south-west when
it means nothing teaches you to stop believing it, and then the feature is
dead.

## Rendering

### Bearing marker

A 1 px radial line from centre to rim on the inbound bearing, with a bold
chevron at the rim. Near-zero occlusion — one pixel per radius step.

The rim mark also resolves a convention problem for free. An arrow can mean
"rain comes *from* here" or "rain is heading *that* way"; on a glanceable
screen you will not remember which you picked, and a 180 deg-ambiguous
indicator is worse than none. **The chevron sits on the rim among the echoes it
describes**, colocated with the stipple it points at. There is nothing to
remember.

### Sweep

10 min/rev, phased on **frame age** — `angle = ((now - frame_time) % 600) / 600
* 360`. Beam at 12 o'clock means a frame just landed; three-quarters round means
the next is ~2.5 minutes out. This is what a PPI sweep means on real hardware:
one revolution, one measurement.

Phase must come from `now - frame_time`, **not from wall-clock 10-minute
phase.** RainViewer publishing late, or the 5-minute refresh lagging detection,
would make "12 o'clock = frame just landed" false by up to half a revolution.
Frame-age phase is honest by construction, and it unifies the sweep with
staleness for free: `STALE_AGE_SEC` is 25 minutes, which is **2.5 revolutions**.

### Liveness

**The sweep is read by position, not by motion.** At 10 min/rev the rim tip
moves 1.55 px/s — roughly 0.33 mm/s on this panel, about 2 arcmin/s at desk
distance. That is at the threshold of human motion detection: perceptible if
you stare, invisible at a glance. It is a clock hand, and a wall clock's minute
hand is not a liveness indicator either.

Two consequences the sweep must not be credited with:

- **It does not signal live-vs-stale at a glance.** That job belongs to the
  `DATA HH:MM` stamp and the `-34M` age footer, which already ship and which
  say *how* stale rather than merely *that* it is.
- **Built note #6's park-at-12-o'clock is deleted, not retained.** A beam
  crawling at 1.55 px/s and a beam frozen at a constant angle are
  indistinguishable, so parking would signal nothing. With frame-age phase the
  beam instead keeps winding past one revolution, and its position past 2.5
  turns *is* the stale condition — no park constant, no separate mechanism.

The trailing wedge must get **narrower and dimmer** than the current
40 deg / 7-of-16. That geometry was tuned for something crossing the eye in
6 seconds; something dwelling in a sector for ~67 seconds must be far more
polite about occlusion.

Built note #6 no longer holds: `frame_time % 60 == 0` was the justification for
parking at a constant, but at 10 min/rev the live beam is genuinely phase-locked
to `frame_time`, so the parked angle can now legitimately derive from it.

### Panel

`+7H` is replaced by the ETA. `+3H` stays.

```
RAIN            <- eyebrow, replaces +7H at baseline 60/102
25M             <- large numerals; states: 25M / NOW / CLEAR / --
----            <- rule at 140
+3H             <- unchanged, 180/222
60%
DATA HH:MM      <- unchanged, 258/272
88 KM | -34M    <- unchanged, 290
```

`+7H` was the least actionable thing on screen: seven hours out, as a
probability, is a question a phone answers better. What remains is a clean
two-slot structure that makes the #5 replacement visible in the layout —
**minutes from radar, hours from Open-Meteo.** Three numbers on a 100 px column
is one too many to take in on a glance, which is the only way this screen is
read.

`CLEAR` beside `+3H 90%` is **not** a contradiction and must not be suppressed:
"nothing inbound within the ~2 h the scope can see, 90% chance in three hours"
is coherent, and is arguably the most useful thing the pair can say together.

The no-vector state is `--`, matching the `"--:--"` placeholder convention
already at `radar_screen.cpp:766`; a blank slot under a `RAIN` eyebrow reads as
a bug. With `+7H` gone, the Open-Meteo query can also drop `forecast_hours=8`
to `4`.

Fit risk: `CLEAR` at `helvB` weight may not fit 100 px. `tools/radar_preview.py`
already reports label fit using real u8g2 glyph advances, so this is measurable
rather than arguable.

## Constants to measure, not guess

Written as named constants with placeholder values, explicitly unmeasured:

- `RADAR_ETA_TIER_FLOOR` (default tier 2, tier 1 is the looser knob)
- `RADAR_CORR_TIER_FLOOR` (default = display floor; distinct from the above)
- `RADAR_CORRIDOR_HALF_PX` (~15)
- `RADAR_MAX_SHIFT_PX` (20)
- the three gate thresholds: minimum wet cells, match fraction floor, peak margin
- `|v|` floor for "stalled" (~10 km/h)

## Unverified

Everything in the Built section's closing note still applies — decode timing,
the frame budget, how the ST7305 holds the dither. Revision 2 adds:

- **Whether confident peaks exist at all** on real weather over this tile. If
  central Thailand's rain is mostly diffuse, or cells develop and decay rather
  than translate over a 10-minute gap, gate 3 rejects nearly every pair and the
  vector is almost never drawn.
- Decision #27 accepts this risk. The offline alternative was a `tools/` probe
  running the gates over the 12 consecutive frame pairs the RainViewer index
  already exposes (13 past frames, 120 min) — the one piece of work that could
  have *falsified* the design rather than elaborating it. It remains available
  if the built version draws vectors that look wrong.
- The failure mode is a screen drawing a confident wrong bearing, not one that
  visibly breaks. Gate 3 is the only defence, and its margin is unmeasured.

## Built (revision 2)

Implemented 2026-08-23. Builds clean against ESP-IDF v5.5.1; **not yet run on
hardware.** Six places where the build departs from Revision 2 above:

1. **Correlation is sampled point-wise, not bit-shifted popcount.** The design
   costed it as "shift, AND, popcount over 2,850 words per candidate offset...
   well under 10 ms". The masks are LSB-first packed rows, so a shifted
   popcount needs per-row bit juggling for every candidate. `corr_matched`
   instead tests bits directly, sampling every second pixel on both axes
   (`RADAR_CORR_STEP`). That is ~12,900 samples per candidate and ~170
   candidates after coarse-to-fine — around 2.2M iterations, once per ten
   minutes, on the refresh path. **Slower than the design's estimate and
   unmeasured on hardware.** If it proves too slow the step is the knob;
   the popcount form is the fallback.

2. **The decoder's floor guard admits either mask.** The early reject is
   `tier < RADAR_TIER_FLOOR && tier < RADAR_CORR_TIER_FLOOR`, not the display
   floor alone. They are equal today, so nothing changes — but raising
   `RADAR_TIER_FLOOR` back to 2 would otherwise silently starve the
   correlation mask, which is exactly the coupling decision #21 exists to
   prevent.

3. **Gate 3's adjacency is one coarse cell, not a pixel radius.** Candidates
   within Chebyshev distance 1 on the coarse grid — ±4 px — are treated as
   part of the winner's own peak rather than rivals to it. Evaluating the gate
   on the coarse grid is deliberate: that is the scale at which a flat
   correlation surface actually looks flat.

4. **The ETA is clamped to 999M.** At the `RADAR_STALLED_KMH` floor a cell at
   the rim is over eight hours out. Unclamped it overflowed the panel buffer,
   which the compiler caught. Matches the age footer's existing convention.

5. **The bearing marker's halo uses one perpendicular for all three lines.**
   The chevron arms sit ±11 deg off the radial, so their true normals differ
   slightly. At a 1 px halo the difference is invisible and it saves two more
   sin/cos per frame.

6. **The sweep is 20 deg at 4/16, half the old width and density.** The design
   said "narrower and dimmer" without numbers. Halving both is the honest
   reading of a beam that now dwells in a sector for over a minute instead of
   crossing it in six seconds.

Also verified during the build: `probability_7h` had no consumers outside this
file, so removing the +7H slot did not touch `http_api.cpp` or the public
header.

**Still unverified, and this is the load-bearing gap:** whether the three gates
pass on real weather over this tile. Nothing in this revision has seen a live
frame pair. The failure mode is a screen drawing a confident wrong bearing, not
one that visibly breaks — see decision #27 and the offline probe it declined.

### Caught in review, before flashing

**The stale path lied.** A dead router leaves `g_radar.motion` holding its last
value while `frame_time` recedes. `draw_eta` would keep decaying `remaining`
past zero into a permanent confident `NOW`, and `draw_bearing` would keep
drawing the chevron — beside a footer quietly reporting `-64M`. No correlation
bug required; a router reboot was enough.

The fix is the Q5 rule applied one level up: **`live` gates the bearing and the
ETA together**, and the `--` state already means "no trustworthy vector". The
value was already computed in `radar_render_current`; it simply never reached
either drawing path.

`tools/radar_preview.py` has **not** been updated for this revision. It still
mirrors the projection, classifier, dither and landmark fit, but its beam
timing and geometry, its panel slots and the bearing marker are all now the old
design. `FEATURE_INDEX.md` says so rather than continuing to claim a
correspondence that no longer holds.

### Revision 2a — the ETA needed a horizon

The build clamped the ETA at 999 minutes, which is a buffer-safety bound, not a
physical one. Combined with `RADAR_STALLED_KMH = 10` the gates permitted
arrival times up to **524 minutes** — and would have drawn a confident chevron
beside them.

What a long ETA actually implies, at a scope radius of 87.3 km:

| Rain sits at | 230-minute ETA implies | Frame-to-frame shift |
|---|---|---|
| 87 km (rim) | 22.8 km/h | **6.4 px** |
| 53 km | 13.9 km/h | 3.9 px |
| 35 km | 9.2 km/h | 2.6 px |

Those displacements sit at the noise floor of a ±20 px search, in exactly the
regime where gate 3 is carrying the whole result — and every gate threshold is
still an unmeasured placeholder. It also runs past this document's own stated
limit: *"Scope shows ~2 h of approach, not 3... Do not label the scope as
showing +3h."* A `230M` reading would render directly above `+3H 60%`, the
radar claiming to time an arrival further out than the forecast slot beneath
it.

`RADAR_STALLED_KMH` does not catch this. It rejects a cell that is not moving,
not one moving too slowly for the arithmetic to stay honest out to the rim.

**Added `RADAR_ETA_HORIZON_SEC = 2 h`, with its own display state `>2H`.**

- Not `CLEAR` — something is genuinely inbound.
- Not a figure — that claims a precision the instrument does not have.
- `>2H` says what is true, and hands the question to the `+3H` slot below,
  which restores the panel's division of labour rather than quietly violating
  it.

Tested on the **decayed** value rather than at scan time, so a measurement that
starts beyond the horizon turns into a real figure by itself once it is close
enough to be credible. The bearing is still drawn: direction remains valid and
useful when only the timing is uncertain, and the "wedge and number vanish
together" rule is not broken because `>2H` is still an answer.

### Revision 2b — polling aligned to the frame clock

Measured against the live index on 2026-08-23 22:21:

- 13 consecutive frames spaced **exactly 600 s**; every stamp on a 10-minute
  boundary.
- Publication trails the stamp by **102 s** (one sample).
- `nowcast` empty, as the original design recorded.

The old poll was free-running at 5 minutes off `esp_timer_get_time()`, so its
phase relative to publication was arbitrary: it asked twice per frame and threw
one answer away.

**Aligning to the boundary itself would be wrong.** At 9:00:00 the 9:00 frame
does not exist yet — the index still reports 8:50, the tile fetch
short-circuits, and the next aligned poll is not until 9:10, so the 9:00 frame
is **skipped entirely**. That also stretches the motion pair to a 20-minute
gap, right on `RADAR_MAX_PAIR_GAP_SEC`.

So the poll is driven by the newest stamp that *should* exist rather than by a
fixed offset:

```
expected = floor((now - PUBLISH_LAG_SEC) / 600) * 600
due      = expected > frame_time
```

Self-correcting in both directions. An early ask is never made. A publish later
than `PUBLISH_LAG_SEC` leaves `expected` still ahead of the held frame, so it
stays due and is retried after `ALIGNED_RETRY_US` (60 s) instead of being lost
until the next period — and those retries are cheap, because the frame_time
short-circuit means only the 818-byte index is fetched, never the tile.

In practice polls land around :02, :12, :22. `REFRESH_INTERVAL_US` survives only
as the pre-SNTP fallback: with no wall clock there is nothing to align to.

Not verified: `PUBLISH_LAG_SEC = 120` rests on a single lag measurement. If the
real distribution has a long tail, the retry path absorbs it at the cost of one
extra index fetch per minute.

### Revision 2c — BEAM_ENABLED

The sweep survived two rounds of questioning (decisions #24, then reaffirmed
after #25 made its original justification void), and what it ended up carrying
is one fact: **its position is a countdown to the next frame.**

That fact is derivable from the `DATA` stamp plus "frames come every ten
minutes", so the sweep saves arithmetic rather than supplying information. The
honest remaining argument for it is that a radar scope without a sweep does not
look like a radar scope — an aesthetic call, and not one that can be settled
off the board.

`BEAM_ENABLED` is therefore a switch, not a rewrite. Both branches build.

| | SPI pushes | Duty at 24 MHz |
|---|---|---|
| As originally shipped (70 ms) | 14.3/s | 7.2% |
| `BEAM_ENABLED = true` | 1/s | 0.5% |
| `BEAM_ENABLED = false` | ~1/min (the ETA ticking) | 0.008% |

15 KB per transfer, 5.0 ms each. Even the sweep-on case is 14x better than what
shipped, so this is not a battery decision — it is a "did you ever actually
look at it" decision, answerable only after a week on the desk.

### Revision 2d — aura, and the sweep back to 60 s

**Aura.** Heavy cores are ringed with a halo that lights as the sweep passes
and fades behind it, the way a PPI scope's phosphor does.

Measured on a live frame before building it:

| tier | px | % of scope |
|---|---|---|
| 0 | 11,903 | 17.31% |
| 1 | 2,542 | 3.70% |
| 2 | 5,916 | 8.60% |
| **3** | **90** | **0.13%** |

84 of those 90 tier-3 pixels have at least six heavy neighbours in a 5x5 —
heavy rain here clusters into real cores rather than scattering, which is what
makes a halo meaningful. tier 2+ would have been 8.73% of the scope, far too
much to ring.

The dither already draws tier 3 at 16/16, so the aura adds emphasis rather than
information. What it buys is legibility: a ~10x10 solid patch inside a 300x300
field carrying 17% tier-0 stipple everywhere is genuinely easy to miss.

Pipeline: `heavy_mask` at decode -> 5x5 cluster filter -> dilate 3 px ->
subtract the cores -> extract to a bounded point list with bearings baked in.
That last step is what makes it cheap: the original design rejected per-pixel
`atan2` because it meant 90,000 pixels per frame, but the ring is ~130 points
and its bearings are computed **once per frame, not once per render**. Render
is then a single angle comparison per point. Oversized rings are subsampled
rather than truncated — a thinned halo still reads as a halo, a halo that stops
halfway round reads as a bug.

**The sweep goes back to 60 s/rev**, and `RENDER_PERIOD_RADAR_MS = 100`.

This reverses Revision 2's #25, and the reason is that the aura gave the sweep
a better job than the one #25 gave it. As a countdown to the next frame the
sweep was carrying a fact derivable from the `DATA` stamp — and in practice
unreadable, since 1.55 px/s is below glance-detectable. As the thing that
**lights the cores**, it has a function you can actually see, and that function
wants a reveal frequent enough to be in front of. Once every ten minutes is a
reveal nobody is present for; once a minute is.

This is **not** a revival of the original "doubles as a minute hand" claim,
which was false and started this whole revision. A 60 s revolution points at
the current second. The sweep is justified by what it does to the aura, not by
anything it says about the time.

| | tip speed | cadence | duty | aura pulse |
|---|---|---|---|---|
| 600 s (rev 2) | 1.55 px/s | 1000 ms | 0.5% | every 10 min |
| **60 s (now)** | **15.5 px/s** | **100 ms** | **5.0%** | **every 1 min** |
| as first shipped | 15.5 px/s | 70 ms | 7.2% | — |

100 ms holds the step at ~1.55 px, the same smoothness already accepted, for
10 pushes a second instead of 14.3. So this costs 10x the sweep-at-600s case
but is still cheaper than what originally shipped.

What is given up, explicitly: the sweep's position no longer means anything,
and at 25 revolutions per staleness threshold it cannot signal staleness
either. The `DATA` stamp and the `-34M` footer carry that alone. `BEAM_ENABLED`
still turns the whole thing off, and the aura then draws flat at 8/16 so the
legibility survives.

### Revision 2e — contacts instead of halos

The halo from 2d ringed the *true footprint* of heavy rain. Replaced with a
**radar contact**: a solid dot at the cluster centroid, with a ring that leaves
it, expands and fades as the sweep goes past — a PPI ping.

Measured on a live frame before rebuilding it, connected components over tier 3
with a 2 px link distance:

| | clusters | >= 8 px | largest |
|---|---|---|---|
| tier 3 | 4 | **2** | 50 px, radius 4.0 px, 45 km out |
| tier 2+ | 31 | 14 | 2,633 px, radius 29 px |

That table is the whole argument. At tier 3 there are two contacts and the
bigger one is **8 px across** — there is no shape for a symbol to hide, so the
abstraction costs nothing. tier 2+ would be a lie as a dot: 2,633 px is storm
*area*, and drawing it as a marker would misrepresent it badly.

The dot is deliberately drawn larger than the returns it stands for
(`RADAR_BLIP_PAD_PX`, then clamped between 4 and 12 px). It is a marker, not a
measurement — the same reason an aircraft blip is not to scale.

Simpler than what it replaced, which is the main reason to prefer it: the halo
needed a dilation mask, a ring mask, a bounded point list and a subsampling
rule for oversized rings. Contacts need a flood fill and at most 16 entries.
The per-render cost went from ~130 angle comparisons to at most 16.

Implementation notes:

- **Flood fill is iterative with an explicit PSRAM stack**, not recursive: an
  88 km scope of heavy rain would otherwise blow the radar task's stack.
  Overflow splits a contact in two rather than corrupting memory.
- **The dot knocks out the field behind it** before drawing, so its edge reads
  as a contact rather than as a patch of heavier stipple.
- **`u8g2_DrawCircle` is solid-only**, so the ping needed a dithered circle
  helper — a ring that cannot fade is not a ping.
- **The dot is drawn unconditionally**, the ring only when swept. Heavy rain
  stays legible with `BEAM_ENABLED = false` or the clock unset.
- When more than `RADAR_BLIP_MAX` contacts exist, the **biggest** are kept: on
  a scope that crowded the small ones are what you can afford to lose.

Unverified, as with everything else here: no live frame has been rendered
through this path, and `RADAR_BLIP_MIN_PX = 8` comes from one measurement of
one frame.

### Revision 2f — the sweep paints the contacts

The dot no longer stands there permanently; the beam lights it and it decays,
which is what a PPI scope actually does.

**Two decay timescales, because one does not work.** Fading the dot on the
ring's 90 deg schedule would leave heavy rain visible for 15 s of every 60 —
a 75% chance of missing it on any given glance, which defeats the entire reason
for marking it. So:

| | travel | at 60 s/rev |
|---|---|---|
| ring ping | 90 deg | 15 s of every 60 |
| dot decay | 270 deg | 45 s of every 60 |

Short and sharp is what makes the ring read as a pulse; long is what keeps the
contact from being a coin flip. The blind window is the quarter turn before the
sweep comes round again.

Two details that follow from fading a dot rather than drawing it solid:

- **The knockout is conditional.** A dark disc under a *solid* dot is what
  gives it a hard edge and makes it a contact rather than a patch of heavier
  stipple. Under a *thinning* dot the same knockout reads as a hole punched in
  the rain, so below `RADAR_BLIP_EDGE_DENSITY` it is dropped and the dot simply
  thins into the field — which is what decay should look like.
- **`u8g2_DrawDisc` is solid-only**, so the fading dot needed a dithered disc
  helper alongside the dithered circle the ring already used.

`BEAM_ENABLED = false` still draws contacts solid and static. Turning off the
sweep should cost the animation, not the information — and with no sweep there
is by definition nothing to paint them.

### Revision 2g — close-contact alert, and a draw-order bug

**The bug.** `draw_blips` ran before `draw_furniture`, so a contact near home
was crossed out by the full-width crosshair and the range rings — exactly the
contact that matters most. Blips now draw *after* the furniture and landmarks.
A contact outranks a range ring, and the landmarks already take that same
liberty. `draw_home_marker` still runs last, so home itself is never hidden by
a contact sitting on top of it.

**The alert.** Two rings around the centre whenever a heavy contact is within
`RADAR_BLIP_NEAR_PX`, sitting just outside the home marker's 15 px crosshair.

The threshold is **37 px — the first range ring, 22 km** — chosen because it is
already drawn on the scope. The alert then needs no legend: it means "there is
a heavy cell inside the inner ring", and the ring it refers to is right there.

Drawn **solid and constant, not pulsed with the sweep.** The contacts decay
because that is what a painted target does, but an alert that blinks out for a
quarter of every minute is an alert you can miss, and this is the one condition
on the scope worth never missing. Same reason it survives
`BEAM_ENABLED = false`.

Both rings use the bearing line's halo trick — a 1 px knockout either side — so
they read over any density of stipple underneath. A filled knockout was
rejected: it would punch a 28 px hole in the rain right where you most want to
see it.

Known limit: distance is **centroid to centre**, so a large cell whose leading
edge is already closer trips the alert late. The ETA is what reports approach;
this only reports arrival in the neighbourhood.

### Revision 2h — first hardware run, two UI fixes

Flashed and running. **The correlation works on real weather over this tile** —
the panel showed `>2H`, which only prints when `motion.valid` is true, so all
three gates passed on live frames. That was the load-bearing unknown the whole
"Tell" branch rested on, and decision #27 chose to test it this way rather than
with an offline probe.

Two things the photo showed that no amount of design could have:

**1. The bearing line was far too long.** Running the full 148 px from centre to
rim, it read as a slash across the entire scope — more ink than the rain it
pointed at, crossing every range ring on the way. Cut to a 30 px tail inward
from the chevron apex, roughly 80% less ink.

Shortened from the *inside*, not the outside, deliberately: the chevron has to
stay at the rim among the echoes it describes, because that colocation is what
removes the from/toward ambiguity without a legend. The tail only has to say
"this is a direction, not a bearing tick". `RADAR_BEARING_APEX_PX` was a bare
14 in the draw code and is now named alongside `RADAR_BEARING_TAIL_PX`.

**2. Landmark labels were hiding the approach.** `draw_landmarks` placed every
label to the *right* of its dot, flipping only when it would run off the scope.
For a westerly landmark that puts the label — and its knockout, which blanks
the field to black — directly in the strip a cell crosses on its way to home.
Rain vanished behind "LAN SAK" exactly as it began to matter.

The rule is now **place the label on the side away from home**, flipping back
only when the outward side does not fit. Verified against all five:

| label | x | was | now |
|---|---|---|---|
| NAKHON SAWAN | 203 | right 208..268 | unchanged |
| UTHAI THANI | 183 | right 188..243 | unchanged |
| SING BURI | 252 | left 202..247 | unchanged (outward does not fit) |
| DAN CHANG | 123 | right 128..173 | **left 73..118** |
| LAN SAK | 101 | right 106..141 | **left 61..96** |

`tools/radar_preview.py` mirrors the new rule, so the one property
FEATURE_INDEX still credits it with — landmark label fit — stays true.

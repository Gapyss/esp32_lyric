# Implementation Handoff: Hydration Reminder ("drink water") — ESP32-S3-RLCD-4.2

**Audience:** an implementing agent starting cold. Read `../spec.md` for the board
(pins, SoC, display) and this file for the feature. This feature is added as a **new mode
inside the existing firmware** (alongside `music_screen`), reusing its WiFi / mDNS / HTTP /
display scaffold.

> **Status:** NOT STARTED. This is a design handoff produced from a grilling session.
> The Now-Playing music feature is already shipped and working — do **not** remove it;
> the hydration reminder is a second mode selectable at build time (and switchable at runtime).

---

## 0. Requirements to use this board's hardware (per peripheral)

> The user asked first for *"the requirements to use this board based on what it has."* This
> feature touches three peripherals. Board baseline (SoC, flash, PSRAM, display) is in `../spec.md`.
> Below is *what you specifically need to drive each part this feature uses.*

### 0a. Display — ST7305 (already working, reuse verbatim)
- Driver: `components/u8g2_st7305` (copied, proven). Init is done in `main.cpp:display_start()`.
- 400×300, 1-bit, `U8G2_R1`, full-frame buffer. Pins MOSI=12 SCK=11 DC=5 CS=40 RST=41 TE=6.
- **You render with on-device u8g2 fonts** for this feature (see §2, decision "text source").

### 0b. Speaker — ES8311 codec, I2S output  ← the headline, and the main risk
What you need to drive it:
| Need | Value / how to get it |
|---|---|
| Control interface | **I2C** — same bus as the RTC: **SDA=GPIO13, SCL=GPIO14, 400 kHz** (see `../spec.md`). ES8311 default 7-bit addr **0x18** (confirm by probe). |
| I2S data pins | **MCLK / BCLK / WS / DIN(→codec) / DOUT(codec→)** — **NOT in the public `board_cfg.txt`.** Recover them per the ordered recipe in **§8** *before writing playback code*. |
| Power-amp enable | **A speaker PA/amp-enable GPIO almost certainly exists** and must be driven high or the codec inits fine and you hear nothing. Hunt for it in the audio demo / `codec_board` (see §8). |
| Format | ES8311 is a standard, widely-supported mono codec. Reference firmware runs I2S **TDM, 16 kHz / 16-bit**; for a simple chime, **I2S standard mode, 16 kHz mono, 16-bit** is fine. |
| Software | ESP-IDF **`espressif/esp_codec_dev`** component (has an ES8311 driver) + `driver/i2s_std` (or `i2s_tdm`). Add to `idf_component.yml`. |

### 0c. Real-time clock — PCF85063 (I2C)
| Need | Value |
|---|---|
| Interface | **I2C** — SDA=GPIO13, SCL=GPIO14 (shared with ES8311 control; no conflict, just probe both). |
| Address | PCF85063 7-bit addr **0x51** (confirm by probe). |
| Init | On boot, **NTP over the existing WiFi → set the PCF85063 → let it free-run.** RTC then keeps wall-clock across reboot / WiFi loss (needed for active-hours + the idle clock). |
| Software | Small hand-rolled I2C driver (BCD registers) or a community PCF85063 component. Time registers are trivial BCD. |

### 0d. Deferred to Phase 2 (picked originally, cut from v1 idle screen)
- **SHTC3** temp/humidity (I2C, same bus) and **battery ADC** (GPIO4, ADC1_CH3, ~÷3 divider).
  Drivers are easy; they are **not** in v1 because the idle screen is clock+countdown only (§2).

---

## 1. What you are building

A **desk hydration reminder**. Standalone by default (works with the Mac off), with an optional
Mac-push override (hybrid).

- **Idle screen** (between reminders): a big **RTC clock** + a **"NEXT DRINK IN ~N min"** countdown.
- **A reminder fires** on a configurable interval (**default 45 min**), but **only inside an
  active-hours window** (default **09:00–18:00**, RTC-gated — no nagging at night).
  When it fires it:
  1. plays a **synthesized chime** through the ES8311 speaker, and
  2. shows **"💧 DRINK WATER"** full-screen on the LCD.
- **Dismiss = auto-timeout only.** After ~**30 s** the chime stops and the screen reverts to idle.
  No physical button, no GPIO input bring-up.
- **Hybrid control (optional):** the Mac can `POST` "drink now", snooze, or reconfigure
  interval/active-hours over the **existing HTTP server** (§4).
- Config (interval, active-hours) **persists in NVS**.

---

## 2. Decisions already made (do NOT relitigate)

| # | Decision | Why |
|---|----------|-----|
| Goal | A hydration reminder ("tell me to drink water") | user's stated goal |
| Headline HW | **Audio-first = ES8311 speaker output** (playback, not capture) | user chose audio-first; a reminder emits sound |
| Mic array | **ES7210 dropped entirely** | reminder emits audio, never captures |
| Alert sound | **Synthesized chime** (PCM generated in code) | zero audio assets/decoder; proves the I2S output path, which is the real risk |
| Dismiss | **Auto-timeout ~30 s only** | no button on this board; keeps scope tight, never stuck-on |
| Architecture | **Hybrid** — on-device timer default, Mac can push/snooze/configure over HTTP | works Mac-off; reuses existing `esp_http_server` |
| Schedule | **Interval (default 45 min) + RTC active-hours window (default 09–18h)** | matches a real desk reminder; gives the RTC a real job |
| Codebase | **New mode in existing firmware** (`water_screen.*` beside `music_screen.*`) | reuse WiFi/mDNS/HTTP/display scaffold; least work |
| Idle screen | **Clock + countdown only** | SHTC3 + battery deferred to Phase 2 |
| **Text source** | **On-device u8g2 fonts** (NOT Mac-side Core Text bitmaps) | all text is ASCII/numeric; the standalone/"Mac-off" property **requires** on-device rendering. Do **not** route hydration text through the bitmap pipeline the music mode uses for Thai. |
| RTC time | **NTP-on-boot → set PCF85063 → free-run** | reliable wall clock without a Mac |

---

## 3. Phased plan (build in this order — Phase 0 is a gate)

### Phase 0 — ES8311 output bring-up **(GATE — do this first)**
Audio-first means if output doesn't work, the feature premise collapses, so de-risk it before
building anything else.
1. Recover I2S pins + PA-enable GPIO via the **ordered recipe in §8** (schematic is the *last* resort).
2. Probe I2C: confirm ES8311 (0x18) and PCF85063 (0x51) ACK on GPIO13/14.
3. Bring up `esp_codec_dev` + I2S, play a **1 kHz test tone for 1 s**.
4. **Observable checks** (§9): I2C ACK, `i2s_channel_write` returns `ESP_OK` for the full buffer,
   PA-enable pin reads asserted. **Then ask the user to confirm they physically hear it.**
   > **If Phase 0's audible check fails, STOP and report** with the register/pin state you found —
   > do not build the reminder logic on an unproven output path.

### Phase 1 — the reminder feature (the deliverable)
5. PCF85063 driver + NTP-on-boot time set.
6. `water_screen` idle render (clock + countdown) using on-device u8g2 fonts.
7. Chime synthesis (`audio_chime`) + the reminder state machine (idle → alerting → timeout → idle),
   interval + active-hours gating, NVS-persisted config.
8. Mode switch in `render_task` + HTTP endpoints (§4).

### Phase 2 — deferred (do NOT build unless asked)
Spoken voice clip, SHTC3 temp/humidity, battery %, physical-button dismiss.

---

## 4. HTTP contract (new endpoints — add to existing `http_api.cpp`)

Reuse the existing server (port 80, `g4pys-company.local`). Same percent-decode gotcha as the
music endpoints (`esp_http_server` does **not** url-decode — reuse the existing `url_decode` helper).

| Endpoint | Method | Purpose |
|---|---|---|
| `/hydrate/now` | GET/POST | Fire a reminder immediately (Mac push). Respond `ok`. |
| `/hydrate/snooze?min=<n>` | GET/POST | Push the next reminder out by `n` minutes. |
| `/hydrate/config?interval=<min>&start=<HH:MM>&end=<HH:MM>` | GET/POST | Set interval + active-hours; persist to NVS. |
| `/hydrate.json` | GET | Debug echo: `{"mode":"water","clock":"14:23","next_in_min":21,"interval":45,"start":"09:00","end":"18:00","alerting":false}` |

(Leave the existing `/nowplaying` + `/usage.json` untouched.)

---

## 5. Integration into the existing firmware (exact points)

The scaffold in `main/main.cpp` is: `nvs → music_screen_init → display_start → render_task
(pinned core 1, 70 ms loop) → wifi_start → mdns_start → http_api_start`. Hook in as follows:

- **`render_task` (`main.cpp:97`)** currently calls `music_render_current(u8, true)`. Add a global
  mode (`APP_MODE_MUSIC` / `APP_MODE_WATER`) and branch: in water mode call `water_render_current(u8)`.
  Default mode is chosen at build time (a `#define` / Kconfig); a `/mode` HTTP call may switch at runtime.
- **`app_main` (`main.cpp:109`)**: after `nvs_flash_init()`, add `water_screen_init()` and (water mode)
  init the I2C bus, PCF85063, and ES8311/I2S. Do audio/RTC init **after** `display_start()` so the
  idle screen can paint even if audio init fails (mirror how display starts before the blocking
  `wifi_start()` today).
- **NTP**: after `wifi_start()` returns (IP acquired), run SNTP once, set the PCF85063, proceed.
- **`main/CMakeLists.txt`**: add `water_screen.cpp audio_chime.cpp pcf85063.cpp` to `SRCS`; add
  `esp_codec_dev driver esp_timer nvs_flash` (driver for I2S/I2C; esp_timer already present) to `REQUIRES`.
- **`main/idf_component.yml`**: add `espressif/esp_codec_dev: '^1.0.0'` (verify latest tag at build).

---

## 6. Files to create / modify

```
firmware/main/
├── main.cpp            ← MODIFY: mode enum, water init, NTP, render_task branch
├── CMakeLists.txt      ← MODIFY: add SRCS + REQUIRES
├── idf_component.yml   ← MODIFY: add esp_codec_dev
├── http_api.cpp / .h   ← MODIFY: add /hydrate/* + /hydrate.json (reuse url_decode)
├── water_screen.cpp/.h ← NEW: state machine + idle render + alert render (on-device fonts)
├── audio_chime.cpp/.h  ← NEW: PCM chime synth + ES8311/I2S playback
├── pcf85063.cpp/.h     ← NEW: I2C RTC driver (BCD get/set) + NTP sync helper
└── app_config.h        ← NEW: mode enum, defaults (interval 45, 09:00–18:00, timeout 30s), NVS keys
```

---

## 7. Module details

### 7a. `water_screen` — state machine + render
State (guard shared fields with a mutex like `music_screen` does):
```c
typedef enum { WATER_IDLE, WATER_ALERTING } WaterState;
struct WaterCtx {
  WaterState state;
  int64_t last_reminder_us;   // esp_timer; when the last interval started
  int64_t alert_started_us;   // when the current alert began (for 30s timeout)
  int      interval_min;      // NVS, default 45
  int      active_start_min;  // minutes-since-midnight, NVS, default 540 (09:00)
  int      active_end_min;    // NVS, default 1080 (18:00)
};
```
- **Tick logic** (called from render task or a dedicated timer): read RTC wall-clock; compute
  `now_min` (minutes since midnight). If `WATER_IDLE` and `interval` elapsed **and**
  `active_start ≤ now_min < active_end` → enter `WATER_ALERTING`, start chime, record `alert_started_us`.
  If `WATER_ALERTING` and `now - alert_started ≥ 30s` → stop chime, back to `WATER_IDLE`,
  reset `last_reminder_us`.
- **`water_render_current(u8)`**:
  - `WATER_IDLE`: big clock (e.g. `u8g2_font_logisoso42_tn` for `HH:MM`) centered high; below it
    **"NEXT DRINK IN  MM:SS"** countdown (`interval` minus elapsed). Small footer `g4pys.company`.
  - `WATER_ALERTING`: full-screen **"💧 DRINK WATER"** (large font; a water-drop can be a small XBM
    or just the text "DRINK WATER"). Optionally a shrinking bar for the 30 s timeout.
  - Reminder: u8g2 `y` is the **baseline**, not top-left (`baseline = top + u8g2_GetAscent(u8)`).
- **Fonts:** pick from bundled u8g2 fonts (numeric `_tn` fonts for the clock; a bold face for the
  alert). No font build step needed — this is all ASCII/digits.

### 7b. `audio_chime` — synth + playback
- **Synthesize** a short pleasant chime into a PCM buffer (int16, 16 kHz mono): e.g. two/three notes
  (C6→E6→G6), each ~150 ms sine with a fast attack + exponential decay envelope to avoid clicks;
  ~0.5 s total. Generate into a static buffer at init (or on the fly).
- **Playback:** write the buffer to the ES8311 via `esp_codec_dev` / `i2s_channel_write`. For the
  30 s alert, loop the chime with a gap (e.g. play every ~3 s) rather than a continuous 30 s tone.
- **PA enable:** assert the amp-enable GPIO before playback, deassert after (saves power on battery).
- Keep a `chime_stop()` so the timeout / HTTP dismiss can cut it off cleanly.

### 7c. `pcf85063` — RTC driver
- I2C (GPIO13/14, 400 kHz). `rtc_get(struct tm*)` / `rtc_set(const struct tm*)` — registers are BCD.
- `rtc_sync_from_ntp()`: after WiFi IP, `esp_sntp` → `localtime` → `rtc_set`. Set TZ for local time.
- If NTP fails (Mac-off / no WiFi), fall back to whatever the RTC already holds (it free-runs).

### 7d. `app_config` — defaults + NVS
- Defaults: `interval=45`, `active=09:00–18:00`, `alert_timeout=30s`, `mode=WATER` (or MUSIC).
- Persist interval + active-hours in NVS (namespace e.g. `"hydrate"`); load at boot, save on `/hydrate/config`.

---

## 8. ES8311 I2S pin + PA-enable recovery (do this in Phase 0, in this order)

The I2S pins are **not** in the public `board_cfg.txt`. **Schematic reverse-engineering is the LAST
resort, not the first step.** Try, in order:
1. **Waveshare official repo** `github.com/waveshareteam/ESP32-S3-RLCD-4.2` →
   `02_Example/Arduino/07_Audio_Test.ino` and the **`codec_board`** component's `board_cfg.txt` /
   `codec_board.h` — this is where every *other* pin in `spec.md` came from. Look for `MCLK/BCLK/
   WS/DIN/DOUT/DO/DI` and a **PA / amp / `SPK_EN` / `PA_EN`** GPIO.
2. **Espressif reference configs** for ES8311 — `esp_codec_dev` examples and the **esp-box** /
   **esp-adf** ES8311 board configs (ES8311 is a standard codec; pin *roles* and register setup match).
3. **Waveshare wiki** `docs.waveshare.com/ESP32-S3-RLCD-4.2` audio section.
4. **Only if all above fail:** read the board schematic PDF from the product page and trace the
   ES8311/ES7210 nets. Record whatever you find back into `spec.md`'s Audio section (it currently
   says "confirm against the schematic" — replace that with the real pins).

**Common "inits fine, no sound" traps:** (a) the **PA-enable GPIO not asserted**; (b) MCLK not
provided/needed; (c) wrong I2S mode (try standard mono first, TDM if the codec_board demands it);
(d) ES8311 register init incomplete — prefer `esp_codec_dev`'s ES8311 driver over hand-rolling regs.

---

## 9. Acceptance checks

> **Audio is not machine-observable by the agent** (it can't hear). Split checks into *observable by
> the agent* and *must be confirmed by the user's ears.*

**Agent-observable (must all pass):**
- [ ] `idf.py build flash monitor` succeeds; boots into the **idle screen** (clock + countdown) even
      with WiFi creds wrong / Mac off. (Doubles as display + mode-switch bring-up.)
- [ ] Serial log: I2C probe shows **ES8311 @0x18 and PCF85063 @0x51 ACK** on GPIO13/14.
- [ ] `i2s_channel_write` (or `esp_codec_dev_write`) returns `ESP_OK` for the full chime buffer; no
      DMA underrun/error logged.
- [ ] The **PA-enable GPIO reads asserted** during playback (log its level).
- [ ] After NTP, the on-screen clock matches wall-clock; RTC read-back is correct; clock keeps
      advancing after a WiFi disconnect (proves free-run).
- [ ] Force a reminder via `curl .../hydrate/now` → screen switches to **"💧 DRINK WATER"**, then
      **auto-reverts to idle after ~30 s** with no input.
- [ ] Reminder does **not** fire outside active hours: set `active=00:00–00:01` via `/hydrate/config`,
      confirm no alert fires; `/hydrate.json` echoes the config.
- [ ] `/hydrate/snooze?min=10` pushes `next_in_min` out by ~10 in `/hydrate.json`.
- [ ] The **music mode still works** (build/flash the music mode, `/nowplaying` still renders).

**Must be confirmed by the user (ears):**
- [ ] The user **physically hears the chime** when a reminder fires (Phase 0 tone, then the real chime).
      *This is the true audio success signal — the agent should explicitly ask the user to confirm.*

---

## 10. Explicitly OUT of scope (do not build)

Mic capture / ES7210 / voice recognition, spoken voice clips, SHTC3 temp/humidity, battery %,
physical-button dismiss, deep-sleep power optimization, OTA, captive portal, removing or breaking
the existing music mode.

---

## 11. Open risks to watch

1. **ES8311 I2S pins unknown** (biggest) — §8 recovery. If step 4 (schematic) is reached and still
   ambiguous, report to the user rather than guessing pins.
2. **PA-enable GPIO** — the classic "everything inits, silence" cause; find and assert it.
3. **Shared I2C bus** — ES8311 control + PCF85063 (+ future SHTC3) all on GPIO13/14; init the bus
   once, share it; probe all addresses.
4. **RTC without NTP** — first boot with no WiFi has an unset clock; show `--:--` and skip
   active-hours gating (fire on interval only) until time is known, so the reminder still works.
</content>
</invoke>

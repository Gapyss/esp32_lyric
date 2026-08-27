"""Shared constants: display geometry, protocol magic, timeouts, brand type."""

from __future__ import annotations

import struct
from typing import Callable



DISPLAY_WIDTH = 400
DISPLAY_HEIGHT = 300
FRAME_BYTES = DISPLAY_WIDTH * DISPLAY_HEIGHT // 8
EXTENSION_HOST = "127.0.0.1"
EXTENSION_PORT = 8765
BOARD_HOST = "0.0.0.0"
BOARD_PORT = 8766
# Default board port per family. The ESP8266 has no mDNS client -- it learns the
# Mac's IP from an HTTP push and dials a port compiled into its firmware
# (LYR_PORT in esp8266/clawdmeter_esp8266/lyrics_stream.cpp) -- so its daemon has
# to keep 8766. The ESP32 discovers the port from the mDNS record, so its daemon
# moves aside and the two can run at once without either board being reflashed.
ESP8266_BOARD_PORT = 8766
ESP32_BOARD_PORT = 8767
# Shared lyrics cache: see LyricsStore for why two processes need this.
SQLITE_BUSY_TIMEOUT_SECONDS = 5.0
NEGATIVE_CACHE_SECONDS = 14 * 24 * 60 * 60
# lrclib.net regularly takes 6-12s to first byte under load (DNS/TLS are fast,
# the server is just slow), so the budget must be generous and timeouts are
# worth one retry -- a timed-out resolve sticks as an error for the whole track.
LRCLIB_TIMEOUT_SECONDS = 15.0
# lrclib sits behind Cloudflare, which sheds bursts of requests with 503 (and
# occasionally 429/502/504). These are transient, so retry them like timeouts.
LRCLIB_RETRY_ATTEMPTS = 3
LRCLIB_RETRY_DELAY_SECONDS = 2.0
LRCLIB_RETRYABLE_STATUS = (429, 502, 503, 504)
LRCLIB_MAX_RETRY_AFTER_SECONDS = 10.0
LRCLIB_USER_AGENT = "g4pys-lyrics-display/0.1"
MIN_SCHEDULE_SWAP_MS = 25
ALBUM_ART_INTRO_SECONDS = 5.0
MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
FRAME_ENVELOPE_MAGIC = b"LYR1"
FRAME_ENVELOPE_VERSION = 1
FRAME_KIND_FULL_NOW = 1
FRAME_KIND_FULL_SCHEDULED = 2
FRAME_KIND_RECT_NOW = 3
FRAME_KIND_RECT_SCHEDULED = 4
FRAME_ENVELOPE_STRUCT = struct.Struct("!4sBBHHHHHHHII")
# Optional RGB565 album-art overlay. Color-capable boards negotiate this with
# ``color=rgb565`` on /board. ART1 intentionally reuses the LYR1 geometry header
# so tiny clients can share one incremental WebSocket parser; its payload is
# big-endian RGB565 (two bytes per pixel), row-major, and is always immediate.
COLOR_ENVELOPE_MAGIC = b"ART1"
COLOR_ENVELOPE_VERSION = 1
COLOR_KIND_RECT_NOW = 3
SEC2_MAGIC = b"SEC2"
SEC2_VERSION = 1
SEC2_TEXT = 1
SEC2_BINARY = 2
SEC2_HEADER_STRUCT = struct.Struct("!4sBBHQI")
SEC2_TAG_BYTES = 32
AUTH_TIMEOUT_SECONDS = 5.0
HEARTBEAT_INTERVAL_SECONDS = 20.0
HEARTBEAT_TIMEOUT_SECONDS = 10.0
BOARD_QUEUE_DEPTH = 8
LYRIC_BASE_SIZE = 30
LYRIC_WRAP_SIZE = 26
LYRIC_MIN_SIZE = 14
LYRIC_BOX_X = 14
LYRIC_BOX_WIDTH = DISPLAY_WIDTH - 28
LYRIC_BAND_TOP_Y = 130
LYRIC_BAND_BOTTOM_Y = 204
LYRIC_SINGLE_BASELINE_Y = 178
LYRIC_SINGLE_HIGHLIGHT_Y = 198
LYRIC_HIGHLIGHT_GAP_Y = LYRIC_SINGLE_HIGHLIGHT_Y - LYRIC_SINGLE_BASELINE_Y

# Brand type from the "Tend" design system (claude.ai/design project 019e2c0f-...):
# Roboto Condensed Bold for hero/display text (title + the active lyric line,
# matching the design system's "Now Playing" card), Roboto Medium for the
# artist line, Roboto Regular for lower-emphasis body copy, Roboto SemiBold
# for tracked-uppercase labels. JetBrains Mono (the brand's numeral face) isn't
# vendored here, so Menlo stands in for tabular time/caption text.
# Tend's type roles rendered in Sukhumvit Set, a macOS system family that
# covers Thai and Latin in one face. Roboto has no Thai glyphs, so mixed
# lyrics used to hit a CoreText cascade seam (Latin=Roboto, Thai=system
# fallback); a single Thai+Latin face removes it. Weights mirror the previous
# Roboto hierarchy (display heaviest → body lightest); the lenient >32
# threshold in pack_1bpp keeps even the Text weight legible on the 1-bit panel.
# Note: Sukhumvit Set has no condensed variant, so the display role loses the
# tighter line budget Roboto Condensed had — long lines marquee-scroll sooner.
BRAND_FONT_NAMES = {
    "display": "Sukhumvit Set Bold",
    # The main lyric is the content, not a heading: one step lighter than the
    # Bold title so the title still reads as the hero, while Semi Bold keeps the
    # lyric crisp under the 1-bit threshold.
    "lyric": "Sukhumvit Set Semi Bold",
    "body": "Sukhumvit Set Text",
    "body_medium": "Sukhumvit Set Medium",
    "label": "Sukhumvit Set Semi Bold",
}
MONO_FONT_NAME = "Menlo"
TRACKING_MEGA = 0.12  # em; matches the design system's eyebrow/label letter-spacing
TRACKING_WIDE = 0.04  # em; matches the design system's mono-caption letter-spacing
# Karaoke hollow text: Core Text stroke width is a percentage of the font point
# size. 3.5% ≈ 1px at the 30px base lyric size — thick enough to survive the
# 1-bit threshold, thin enough to keep letter counters open. Smaller wrapped
# sizes bump the percentage so the outline never falls below ~1px.
KARAOKE_STROKE_PCT = 3.5

LyricBreakFn = Callable[[str, float, int], list[str]]

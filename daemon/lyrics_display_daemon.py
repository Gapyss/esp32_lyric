#!/usr/bin/env python3
"""Local daemon for the browser-extension synced-lyrics display design.

This is the first implementation slice for lyrics-display-design.md:

- listens for the browser extension on localhost WebSocket port 8765;
- tracks playback using extension ticks plus local interpolation;
- resolves lyrics through manual/cache SQLite rows, then lrclib;
- renders a full 400x300 1-bpp frame on macOS with Core Text when available;
- exposes a board-facing WebSocket on port 8766 and sends self-describing
  binary framebuffer envelopes;
- advertises the board endpoint as _lyrics._tcp through a stdlib mDNS responder
  with dns-sd fallback.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hmac
import hashlib
import json
import math
import os
import re
import shutil
import signal
import socket
import sqlite3
import struct
import subprocess
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable
from urllib.parse import parse_qs, urlparse
import uuid


DISPLAY_WIDTH = 400
DISPLAY_HEIGHT = 300
FRAME_BYTES = DISPLAY_WIDTH * DISPLAY_HEIGHT // 8
EXTENSION_HOST = "127.0.0.1"
EXTENSION_PORT = 8765
BOARD_HOST = "0.0.0.0"
BOARD_PORT = 8766
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


@dataclass(frozen=True)
class DaemonIdentity:
    daemon_uuid: str
    token: str


def default_identity_path() -> Path:
    return Path.home() / ".g4pys" / "lyrics-identity.json"


def load_or_create_identity(path: Path | None = None) -> DaemonIdentity:
    """Load the long-lived daemon UUID and pairing secret, creating them safely."""
    path = path or default_identity_path()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        os.chmod(path.parent, 0o700)
    except OSError:
        pass
    if path.exists():
        raw = json.loads(path.read_text(encoding="utf-8"))
        daemon_uuid = str(uuid.UUID(str(raw["daemon_uuid"])))
        token = str(raw["token"])
        if not re.fullmatch(r"[0-9a-f]{64}", token):
            raise ValueError(f"invalid token in {path}")
        os.chmod(path, 0o600)
        return DaemonIdentity(daemon_uuid, token)

    identity = DaemonIdentity(str(uuid.uuid4()), secrets.token_hex(32))
    payload = (json.dumps({"version": 1, "daemon_uuid": identity.daemon_uuid, "token": identity.token}, indent=2) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    return identity


def _auth_transcript(role: str, daemon_uuid: str, server_nonce: bytes, client_nonce: bytes) -> bytes:
    return b"lyrics-v2/" + role.encode("ascii") + b"\0" + daemon_uuid.encode("ascii") + b"\0" + server_nonce + client_nonce


def derive_session_keys(token: str, server_nonce: bytes, client_nonce: bytes) -> tuple[bytes, bytes]:
    """RFC 5869 HKDF-SHA256; returns (client->server, server->client)."""
    salt = server_nonce + client_nonce
    prk = hmac.new(salt, token.encode("ascii"), hashlib.sha256).digest()
    output = b""
    previous = b""
    counter = 1
    while len(output) < 64:
        previous = hmac.new(prk, previous + b"lyrics-v2/session" + bytes([counter]), hashlib.sha256).digest()
        output += previous
        counter += 1
    return output[:32], output[32:64]


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


@dataclass
class TrackInfo:
    video_id: str = ""
    title: str = ""
    artist: str = ""
    album: str = ""
    duration_sec: float = 0.0
    art_url: str = ""

    @property
    def key(self) -> str:
        return self.video_id or "\x00".join(
            (self.title.lower(), self.artist.lower(), str(round(self.duration_sec or 0)))
        )


@dataclass(frozen=True)
class CoverArt:
    """A track's album art decoded for mono and color boards.

    ``bits`` holds one byte per pixel (0 or 1, row-major, top-origin), lit=1.
    The renderer blits it into a reserved square rect after the theme decision,
    so a photo keeps its natural tonality in both light and dark themes.
    ``rgb565`` is the same image as big-endian RGB565. It is optional so old
    cache fixtures and geometry-only renderers remain compatible.
    """

    size: int
    bits: bytes
    rgb565: bytes = b""


@dataclass
class PlaybackClock:
    position_sec: float = 0.0
    paused: bool = True
    playback_rate: float = 1.0
    base_monotonic_ms: int = field(default_factory=monotonic_ms)
    frozen: bool = False

    def update(self, position_sec: float, paused: bool, playback_rate: float) -> None:
        self.position_sec = max(0.0, float(position_sec or 0.0))
        self.paused = bool(paused)
        self.playback_rate = float(playback_rate or 1.0)
        self.base_monotonic_ms = monotonic_ms()
        self.frozen = False

    def interpolated_position(self) -> float:
        if self.paused or self.frozen:
            return self.position_sec
        elapsed = (monotonic_ms() - self.base_monotonic_ms) / 1000.0
        return max(0.0, self.position_sec + elapsed * self.playback_rate)


@dataclass
class Lyrics:
    synced: list[tuple[int, str]] = field(default_factory=list)
    syllables: dict[int, list[tuple[int, str]]] = field(default_factory=dict)
    plain: list[str] = field(default_factory=list)
    instrumental: bool = False
    resolved: bool = False
    error: str = ""


@dataclass
class AppState:
    track: TrackInfo = field(default_factory=TrackInfo)
    clock: PlaybackClock = field(default_factory=PlaybackClock)
    lyrics: Lyrics = field(default_factory=Lyrics)
    resolving_key: str = ""
    dirty: bool = True
    cover: CoverArt | None = None
    # Full-screen album art used for the five-second track intro and while
    # paused (decoded at a larger size than the small header cover). None when
    # art is unavailable -> both states fall back to the normal lyrics layout.
    hero_cover: CoverArt | None = None

    def in_album_art_intro(self) -> bool:
        """True while a playing track is inside its first five seconds."""
        return (
            bool(self.track.title)
            and not self.clock.paused
            and self.clock.playback_rate > 0
            and self.clock.interpolated_position() < ALBUM_ART_INTRO_SECONDS
        )

    def active_line_index(self) -> int:
        if not self.lyrics.synced:
            return -1
        pos_ms = int(self.clock.interpolated_position() * 1000)
        active = -1
        for idx, (line_ms, _line) in enumerate(self.lyrics.synced):
            if line_ms <= pos_ms:
                active = idx
            else:
                break
        return active

    def current_lines(self) -> tuple[str, str, str]:
        if not self.track.title:
            return ("", "", "")
        if self.lyrics.instrumental:
            return ("Instrumental", "", "")
        if self.lyrics.synced:
            active = self.active_line_index()
            if active < 0:
                return ("", "", "")
            lines = [line for _line_ms, line in self.lyrics.synced[active : active + 3]]
            return tuple((lines + ["", "", ""])[:3])  # type: ignore[return-value]
        if self.lyrics.plain:
            return (self.lyrics.plain[0], self.lyrics.plain[1] if len(self.lyrics.plain) > 1 else "", "")
        if self.lyrics.error:
            return (self.lyrics.error, "", "")
        if self.resolving_key == self.track.key and not self.lyrics.resolved:
            return ("Loading lyrics...", "", "")
        return ("", "", "")

    def current_line_highlight_fraction(self) -> float:
        if not self.lyrics.synced:
            return 0.0
        active = self.active_line_index()
        if active < 0:
            return 0.0
        start_ms, _line = self.lyrics.synced[active]
        pos_ms = int(self.clock.interpolated_position() * 1000)
        syllables = self.lyrics.syllables.get(start_ms) or []
        if syllables:
            active_syllables = [item for item in syllables if item[0] <= pos_ms]
            return clamp(len(active_syllables) / max(1, len(syllables)), 0.0, 1.0)
        end_ms = self.lyrics.synced[active + 1][0] if active + 1 < len(self.lyrics.synced) else start_ms + 4000
        if end_ms <= start_ms:
            return 1.0
        return clamp((pos_ms - start_ms) / (end_ms - start_ms), 0.0, 1.0)

    def next_line_time_ms(self) -> int | None:
        if self.clock.paused or self.clock.playback_rate <= 0 or not self.lyrics.synced:
            return None
        active = self.active_line_index()
        next_idx = active + 1
        if next_idx >= len(self.lyrics.synced):
            return None
        next_ms = self.lyrics.synced[next_idx][0]
        pos_ms = int(self.clock.interpolated_position() * 1000)
        return next_ms if next_ms > pos_ms else None

    def at_position(self, position_sec: float) -> "AppState":
        return AppState(
            track=self.track,
            clock=PlaybackClock(
                position_sec=position_sec,
                paused=self.clock.paused,
                playback_rate=self.clock.playback_rate,
                frozen=True,
            ),
            lyrics=self.lyrics,
            resolving_key=self.resolving_key,
            dirty=False,
            cover=self.cover,
            hero_cover=self.hero_cover,
        )


@dataclass
class ScheduledFrame:
    frames: dict[str, bytes]  # profile name -> rendered frame
    swap_in_ms: int
    due_monotonic_ms: int


@dataclass
class DirtyRect:
    x: int
    y: int
    width: int
    height: int
    row_bytes: int
    payload: bytes

    @property
    def bytes(self) -> int:
        return len(self.payload)


@dataclass(frozen=True)
class ColorRect:
    x: int
    y: int
    width: int
    height: int
    payload: bytes

    @property
    def row_bytes(self) -> int:
        return self.width * 2


@dataclass(frozen=True)
class LyricLayoutSizes:
    base_size: int = LYRIC_BASE_SIZE
    wrap_size: int = LYRIC_WRAP_SIZE
    min_size: int = LYRIC_MIN_SIZE
    band_top_y: int = LYRIC_BAND_TOP_Y
    single_baseline_y: int = LYRIC_SINGLE_BASELINE_Y
    single_highlight_y: int = LYRIC_SINGLE_HIGHLIGHT_Y
    highlight_gap_y: int = LYRIC_HIGHLIGHT_GAP_Y


@dataclass(frozen=True)
class LyricLayout:
    font_size: float
    rows: list[str]
    baselines: list[int]
    bottom_row_baseline_y: int
    highlight_y: int
    hide_third: bool


DEFAULT_LYRIC_LAYOUT_SIZES = LyricLayoutSizes()

# Compact 240x240 lyric band (ESP8266 GeekMagic SmallTV boards). Same fitting
# algorithm as the 400x300 band, just smaller sizes and a tighter box.
SQUARE_LYRIC_LAYOUT_SIZES = LyricLayoutSizes(
    base_size=22,
    wrap_size=18,
    min_size=12,
    band_top_y=104,
    single_baseline_y=146,
    single_highlight_y=162,
    highlight_gap_y=16,
)


@dataclass(frozen=True)
class RenderProfile:
    """One board display geometry the daemon can render frames for.

    Boards opt into a profile with /board?w=&h= at handshake time; the default
    (no query) stays the original 400x300 e-ink layout so existing ESP32
    firmware needs no change.
    """

    name: str
    width: int
    height: int
    lyric_sizes: LyricLayoutSizes
    lyric_box_x: int
    lyric_box_width: int
    lyric_band_bottom_y: int

    @property
    def row_bytes(self) -> int:
        return self.width // 8

    @property
    def frame_bytes(self) -> int:
        return self.width * self.height // 8


WIDE_PROFILE = RenderProfile(
    name="400x300",
    width=DISPLAY_WIDTH,
    height=DISPLAY_HEIGHT,
    lyric_sizes=DEFAULT_LYRIC_LAYOUT_SIZES,
    lyric_box_x=LYRIC_BOX_X,
    lyric_box_width=LYRIC_BOX_WIDTH,
    lyric_band_bottom_y=LYRIC_BAND_BOTTOM_Y,
)
SQUARE_PROFILE = RenderProfile(
    name="240x240",
    width=240,
    height=240,
    lyric_sizes=SQUARE_LYRIC_LAYOUT_SIZES,
    lyric_box_x=12,
    lyric_box_width=240 - 24,
    lyric_band_bottom_y=184,
)
DEFAULT_PROFILE = WIDE_PROFILE
PROFILES_BY_SIZE = {
    (WIDE_PROFILE.width, WIDE_PROFILE.height): WIDE_PROFILE,
    (SQUARE_PROFILE.width, SQUARE_PROFILE.height): SQUARE_PROFILE,
}


@dataclass(frozen=True)
class ProgressGeom:
    """Coordinates for the elapsed/bar/remaining progress row (top-origin y)."""

    time_size: int
    left_x: int
    left_w: int
    right_x: int
    right_w: int
    baseline_y: int
    bar_x: int
    bar_w: int
    center_y: int  # track centerline, top-origin
    dot_r: float


WIDE_PROGRESS_GEOM = ProgressGeom(
    time_size=13, left_x=14, left_w=54, right_x=328, right_w=58,
    baseline_y=116, bar_x=76, bar_w=238, center_y=109, dot_r=4.5,
)
SQUARE_PROGRESS_GEOM = ProgressGeom(
    time_size=11, left_x=12, left_w=48, right_x=180, right_w=48,
    baseline_y=95, bar_x=66, bar_w=108, center_y=91, dot_r=3.5,
)

# Album-art cover: a dithered square in the header's top-right, above the
# progress row, wrapped in a Tend hairline "card" frame. Title/artist reflow
# into the left column when a cover is present; the karaoke lyric band below is
# untouched. One placement per board profile (the ESP32 e-ink is 400x300; the
# ESP8266 SmallTV is 240x240).
@dataclass(frozen=True)
class CoverPlacement:
    x: int          # art top-left (top-origin), the reserved blit rect
    y: int
    size: int       # square art edge, in pixels
    title_w: int    # header left-column width for title/artist when art present
    frame_pad: int  # gap between art edge and the hairline card frame


COVER_PLACEMENTS = {
    "400x300": CoverPlacement(x=324, y=32, size=60, title_w=324 - 14 - 8, frame_pad=2),
    "240x240": CoverPlacement(x=174, y=30, size=52, title_w=174 - 12 - 8, frame_pad=2),
}


def cover_placement(profile: "RenderProfile") -> CoverPlacement | None:
    return COVER_PLACEMENTS.get(profile.name)


def max_cover_size(profiles: "list[RenderProfile]") -> int:
    sizes = [p.size for p in (cover_placement(pr) for pr in profiles) if p is not None]
    return max(sizes) if sizes else 0


def hero_cover_size(profiles: "list[RenderProfile]") -> int:
    """Decode size for the full-screen art: the largest square that fits
    any connected board that supports covers (400x300 -> 300, 240x240 -> 240).

    Decoded once at the max across boards; each board centers and clips this
    single cover, so a board smaller than the decode size simply crops the edges
    (still full-bleed) while the board it was sized for gets an exact fit.
    """
    sizes = [min(pr.width, pr.height) for pr in profiles if cover_placement(pr) is not None]
    return max(sizes) if sizes else 0


def blit_cover_centered(frame: bytes, cover: CoverArt, profile: RenderProfile) -> bytes:
    """Assign a dithered square cover centered on the frame, clipped to bounds.

    Same bit convention as blit_cover (byte = x//8, bit = 1 << (x & 7), lit=set),
    but centered rather than placed in a header rect. When the cover is larger
    than the frame it crops (full-bleed); when smaller it leaves the surrounding
    background untouched (the caller pre-fills it, e.g. black side bars).
    """
    buf = bytearray(frame)
    row_bytes = profile.width // 8
    size = cover.size
    origin_x = (profile.width - size) // 2
    origin_y = (profile.height - size) // 2
    for row in range(size):
        fy = origin_y + row
        if not (0 <= fy < profile.height):
            continue
        base = fy * row_bytes
        src_row = row * size
        for col in range(size):
            fx = origin_x + col
            if not (0 <= fx < profile.width):
                continue
            mask = 1 << (fx & 7)
            idx = base + (fx >> 3)
            if cover.bits[src_row + col]:
                buf[idx] |= mask
            else:
                buf[idx] &= ~mask & 0xFF
    return bytes(buf)


def composite_chip(
    frame: bytes,
    value: bytes,
    chip_mask: bytes,
    x: int,
    y: int,
    chip_w: int,
    chip_h: int,
    profile: RenderProfile,
) -> bytes:
    """Stamp a rounded PAUSED chip over the art at top-origin (x, y).

    ``value`` and ``chip_mask`` are 1-bpp row-packed bitmaps of the same size:
    ``value`` is the white pill with black label text; ``chip_mask`` is the pill
    coverage (lit = inside pill). Only masked pixels are written, so the pill's
    rounded corners keep the album art behind them instead of a black box.
    """
    buf = bytearray(frame)
    row_bytes = profile.width // 8
    chip_row_bytes = (chip_w + 7) // 8
    for cy in range(chip_h):
        fy = y + cy
        if not (0 <= fy < profile.height):
            continue
        base = fy * row_bytes
        src = cy * chip_row_bytes
        for cx in range(chip_w):
            fx = x + cx
            if not (0 <= fx < profile.width):
                continue
            if not (chip_mask[src + (cx >> 3)] & (1 << (cx & 7))):
                continue
            mask = 1 << (fx & 7)
            idx = base + (fx >> 3)
            if value[src + (cx >> 3)] & (1 << (cx & 7)):
                buf[idx] |= mask
            else:
                buf[idx] &= ~mask & 0xFF
    return bytes(buf)

# Domains the daemon will fetch cover art from. A browser page hands us the URL,
# so we restrict fetches to Google's public image CDNs (SSRF hygiene: never
# fetch an arbitrary URL, even over localhost). YT Music's mediaSession artwork
# is served from assorted googleusercontent.com / ggpht.com subdomains (lh3,
# yt3, ...), so we suffix-match the base domain rather than exact hostnames --
# otherwise the art is silently dropped and we fall back to the video-id
# thumbnail, which only resolves on the watch page (?v=...).
COVER_ALLOWED_DOMAINS = (
    "googleusercontent.com",
    "ggpht.com",
    "ytimg.com",
    "youtube.com",
)
COVER_FETCH_TIMEOUT_SECONDS = 6.0
COVER_MAX_BYTES = 4 * 1024 * 1024


def cover_host_allowed(url: str) -> bool:
    try:
        parsed = urlparse(url)
    except ValueError:
        return False
    if parsed.scheme != "https":
        return False
    host = (parsed.hostname or "").lower()
    return any(host == d or host.endswith("." + d) for d in COVER_ALLOWED_DOMAINS)


def cover_url_for(track: "TrackInfo") -> str | None:
    """Pick a fetchable cover URL for a track, or None.

    Prefers the page-supplied ``art_url`` (mediaSession artwork -- a clean square
    cover, available on any page while something plays) when it is on an allowed
    Google image CDN, and otherwise derives a thumbnail from the video id, which
    is only present on the watch page.
    """
    url = (track.art_url or "").strip()
    if url and cover_host_allowed(url):
        return url
    if track.video_id:
        return f"https://i.ytimg.com/vi/{urllib.parse.quote(track.video_id)}/hqdefault.jpg"
    return None


def fetch_cover_bytes(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": LRCLIB_USER_AGENT})
    with urllib.request.urlopen(req, timeout=COVER_FETCH_TIMEOUT_SECONDS) as resp:
        return resp.read(COVER_MAX_BYTES + 1)[:COVER_MAX_BYTES]


def floyd_steinberg_1bit(gray: list[int], width: int, height: int) -> bytes:
    """Error-diffusion dither of an 8-bit grayscale buffer to 1-bit (lit=1)."""
    buf = [float(v) for v in gray]
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            i = y * width + x
            old = buf[i]
            new = 255.0 if old >= 128.0 else 0.0
            out[i] = 1 if new else 0
            err = old - new
            if x + 1 < width:
                buf[i + 1] += err * 7 / 16
            if y + 1 < height:
                if x > 0:
                    buf[i + width - 1] += err * 3 / 16
                buf[i + width] += err * 5 / 16
                if x + 1 < width:
                    buf[i + width + 1] += err * 1 / 16
    return bytes(out)


def blit_cover(frame: bytes, cover: CoverArt, profile: RenderProfile) -> bytes:
    """Assign the cover's 1-bit pixels into the profile's reserved header rect.

    Assigns (clears then sets) each pixel so a dithered cover's 0-bits also land
    -- a plain OR would leave stale lit pixels showing through dark art. Bit
    order matches pack_1bpp: byte = x//8, bit = 1 << (x & 7), lit = set.
    """
    place = cover_placement(profile)
    if place is None:
        return frame
    buf = bytearray(frame)
    row_bytes = profile.width // 8
    size = cover.size
    origin_x = place.x + (place.size - size) // 2
    origin_y = place.y + (place.size - size) // 2
    for row in range(size):
        fy = origin_y + row
        if not (place.y <= fy < place.y + place.size):
            continue
        if not (0 <= fy < profile.height):
            continue
        base = fy * row_bytes
        src_row = row * size
        for col in range(size):
            fx = origin_x + col
            if not (place.x <= fx < place.x + place.size):
                continue
            if not (0 <= fx < profile.width):
                continue
            mask = 1 << (fx & 7)
            idx = base + (fx >> 3)
            if cover.bits[src_row + col]:
                buf[idx] |= mask
            else:
                buf[idx] &= ~mask & 0xFF
    return bytes(buf)


def color_cover_rect(
    cover: CoverArt,
    profile: RenderProfile,
    *,
    slot_x: int,
    slot_y: int,
    slot_width: int,
    slot_height: int,
) -> ColorRect | None:
    """Center and clip a cover into a screen-space slot as RGB565.

    This deliberately does not scale: covers are decoded at the largest size
    needed by connected profiles, so a smaller profile center-crops the shared
    decode just like the 1-bit hero path. The returned bytes include only the
    visible intersection and can be streamed straight to a TFT one row at a
    time without a color framebuffer.
    """
    size = cover.size
    if size <= 0 or len(cover.rgb565) != size * size * 2:
        return None
    origin_x = slot_x + (slot_width - size) // 2
    origin_y = slot_y + (slot_height - size) // 2
    left = max(0, slot_x, origin_x)
    top = max(0, slot_y, origin_y)
    right = min(profile.width, slot_x + slot_width, origin_x + size)
    bottom = min(profile.height, slot_y + slot_height, origin_y + size)
    if right <= left or bottom <= top:
        return None
    width = right - left
    height = bottom - top
    src_x = left - origin_x
    src_y = top - origin_y
    payload = bytearray(width * height * 2)
    dst = 0
    for row in range(height):
        start = ((src_y + row) * size + src_x) * 2
        count = width * 2
        payload[dst : dst + count] = cover.rgb565[start : start + count]
        dst += count
    return ColorRect(left, top, width, height, bytes(payload))


def composite_rgb565_chip(
    rect: ColorRect,
    value: bytes,
    chip_mask: bytes,
    chip_x: int,
    chip_y: int,
    chip_w: int,
    chip_h: int,
    *,
    paper: int = 0xFF9C,
    ink: int = 0x18E2,
) -> ColorRect:
    """Apply the mono PAUSED pill to an RGB565 cover using Tend colors."""
    payload = bytearray(rect.payload)
    chip_row_bytes = (chip_w + 7) // 8
    left = max(rect.x, chip_x)
    top = max(rect.y, chip_y)
    right = min(rect.x + rect.width, chip_x + chip_w)
    bottom = min(rect.y + rect.height, chip_y + chip_h)
    for sy in range(top, bottom):
        cy = sy - chip_y
        for sx in range(left, right):
            cx = sx - chip_x
            bit = 1 << (cx & 7)
            src = cy * chip_row_bytes + (cx >> 3)
            if not (chip_mask[src] & bit):
                continue
            color = paper if value[src] & bit else ink
            dst = ((sy - rect.y) * rect.width + (sx - rect.x)) * 2
            payload[dst] = color >> 8
            payload[dst + 1] = color & 0xFF
    return ColorRect(rect.x, rect.y, rect.width, rect.height, bytes(payload))


def profile_from_board_path(path: str) -> RenderProfile:
    query = parse_qs(urlparse(path).query)
    try:
        width = int((query.get("w") or ["0"])[0])
        height = int((query.get("h") or ["0"])[0])
    except ValueError:
        return DEFAULT_PROFILE
    return PROFILES_BY_SIZE.get((width, height), DEFAULT_PROFILE)


def board_wants_rgb565(path: str) -> bool:
    query = parse_qs(urlparse(path).query)
    return (query.get("color") or [""])[0].lower() == "rgb565"


def fit_lyric_layout(
    text: str,
    box_w: int,
    box_h: int,
    sizes: LyricLayoutSizes,
    break_fn: LyricBreakFn,
) -> LyricLayout:
    """Choose current-lyric size and rows; break-point quality belongs to break_fn.

    Unit tests inject a fake breaker to cover control flow. The production
    CoreText breaker is what validates Thai/CJK/Latin shaping and break points.
    """
    if not text:
        return _lyric_layout([], sizes.base_size, box_h, sizes)

    rows = _layout_break_rows(text, sizes.base_size, box_w, break_fn)
    if len(rows) <= 1:
        return _lyric_layout(rows, sizes.base_size, box_h, sizes)

    for size in range(sizes.base_size - 1, sizes.wrap_size - 1, -1):
        rows = _layout_break_rows(text, size, box_w, break_fn)
        if len(rows) <= 1:
            return _lyric_layout(rows, size, box_h, sizes)

    rows = _layout_break_rows(text, sizes.wrap_size, box_w, break_fn)
    if len(rows) <= 2:
        return _lyric_layout(rows, sizes.wrap_size, box_h, sizes)

    best_rows = rows
    for size in range(sizes.wrap_size - 1, sizes.min_size - 1, -1):
        rows = _layout_break_rows(text, size, box_w, break_fn)
        best_rows = rows
        if len(rows) <= 2:
            return _lyric_layout(rows, size, box_h, sizes)

    return _lyric_layout(_collapse_to_two_rows(best_rows), sizes.min_size, box_h, sizes)


def _layout_break_rows(text: str, size: float, box_w: int, break_fn: LyricBreakFn) -> list[str]:
    rows = [row.strip() for row in break_fn(text, float(size), box_w) if row.strip()]
    return rows or ([text] if text else [])


def _collapse_to_two_rows(rows: list[str]) -> list[str]:
    if len(rows) <= 2:
        return rows
    return [rows[0], " ".join(row for row in rows[1:] if row)]


def _lyric_layout(rows: list[str], size: float, box_h: int, sizes: LyricLayoutSizes) -> LyricLayout:
    row_count = len(rows)
    baselines = _lyric_baselines(row_count, size, box_h, sizes)
    bottom_row_y = baselines[-1] if baselines else sizes.single_baseline_y
    band_bottom_y = sizes.band_top_y + box_h
    highlight_y = (
        sizes.single_highlight_y
        if row_count <= 1
        else min(bottom_row_y + sizes.highlight_gap_y, band_bottom_y)
    )
    return LyricLayout(
        font_size=float(size),
        rows=rows,
        baselines=baselines,
        bottom_row_baseline_y=bottom_row_y,
        highlight_y=highlight_y,
        hide_third=row_count > 1,
    )


def _lyric_baselines(row_count: int, size: float, box_h: int, sizes: LyricLayoutSizes) -> list[int]:
    if row_count <= 0:
        return []
    if row_count == 1:
        return [sizes.single_baseline_y]
    line_height = max(int(math.ceil(size * 1.22)), int(math.ceil(size + 4)))
    block_height = line_height * row_count
    block_top = sizes.band_top_y + max(0.0, (box_h - block_height) / 2.0)
    baseline_offset = min(line_height - 1, int(round(size * 0.82)))
    return [int(round(block_top + baseline_offset + idx * line_height)) for idx in range(row_count)]


def dirty_rect(
    previous: bytes | None,
    current: bytes,
    width: int = DISPLAY_WIDTH,
    height: int = DISPLAY_HEIGHT,
) -> DirtyRect | None:
    if previous is None or len(previous) != len(current) or previous == current:
        return None
    display_row_bytes = width // 8
    min_row = height
    max_row = -1
    min_byte = display_row_bytes
    max_byte = -1
    for y in range(height):
        row_off = y * display_row_bytes
        for bx in range(display_row_bytes):
            idx = row_off + bx
            if previous[idx] != current[idx]:
                min_row = min(min_row, y)
                max_row = max(max_row, y)
                min_byte = min(min_byte, bx)
                max_byte = max(max_byte, bx)
    if max_row < 0:
        return None
    row_bytes = max_byte - min_byte + 1
    payload = bytearray(row_bytes * (max_row - min_row + 1))
    out = 0
    for y in range(min_row, max_row + 1):
        start = y * display_row_bytes + min_byte
        payload[out : out + row_bytes] = current[start : start + row_bytes]
        out += row_bytes
    return DirtyRect(
        x=min_byte * 8,
        y=min_row,
        width=min(row_bytes * 8, width - min_byte * 8),
        height=max_row - min_row + 1,
        row_bytes=row_bytes,
        payload=bytes(payload),
    )


def make_frame_envelope(
    kind: int,
    payload: bytes,
    *,
    x: int,
    y: int,
    rect_width: int,
    rect_height: int,
    row_bytes: int,
    swap_in_ms: int,
    display_width: int = DISPLAY_WIDTH,
    display_height: int = DISPLAY_HEIGHT,
) -> bytes:
    header = FRAME_ENVELOPE_STRUCT.pack(
        FRAME_ENVELOPE_MAGIC,
        FRAME_ENVELOPE_VERSION,
        kind,
        display_width,
        display_height,
        x,
        y,
        rect_width,
        rect_height,
        row_bytes,
        max(0, int(swap_in_ms)),
        len(payload),
    )
    return header + payload


def make_color_envelope(rect: ColorRect, profile: RenderProfile) -> bytes:
    header = FRAME_ENVELOPE_STRUCT.pack(
        COLOR_ENVELOPE_MAGIC,
        COLOR_ENVELOPE_VERSION,
        COLOR_KIND_RECT_NOW,
        profile.width,
        profile.height,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        rect.row_bytes,
        0,
        len(rect.payload),
    )
    return header + rect.payload


class LyricsStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.conn = sqlite3.connect(str(self.path), check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        with self.lock:
            self.conn.execute(
                """
                CREATE TABLE IF NOT EXISTS lyrics (
                  video_id     TEXT NOT NULL,
                  source       TEXT NOT NULL,
                  synced       INTEGER NOT NULL,
                  instrumental INTEGER NOT NULL DEFAULT 0,
                  lrc          TEXT,
                  title        TEXT,
                  artist       TEXT,
                  album        TEXT,
                  duration_sec INTEGER,
                  fetched_at   INTEGER,
                  PRIMARY KEY (video_id, source)
                )
                """
            )
            self.conn.commit()

    def manual_or_cached(self, track: TrackInfo) -> Lyrics | None:
        if not track.key:
            return None
        with self.lock:
            manual = self.conn.execute(
                "SELECT * FROM lyrics WHERE video_id = ? AND source = 'manual'", (track.key,)
            ).fetchone()
            if manual:
                return row_to_lyrics(manual)

            cached = self.conn.execute(
                "SELECT * FROM lyrics WHERE video_id = ? AND source = 'lrclib'", (track.key,)
            ).fetchone()
            if cached:
                if cached["lrc"] is None and int(time.time()) - int(cached["fetched_at"] or 0) > NEGATIVE_CACHE_SECONDS:
                    pass
                else:
                    return row_to_lyrics(cached)
        return self.legacy_cache_lookup(track)

    def legacy_cache_lookup(self, track: TrackInfo) -> Lyrics | None:
        if not track.title:
            return None
        with self.lock:
            try:
                exact = self.conn.execute(
                    """
                    SELECT synced_lyrics, plain_lyrics, instrumental
                    FROM lyrics_cache
                    WHERE key = ?
                    """,
                    (legacy_lyrics_cache_key(track),),
                ).fetchone()
            except sqlite3.Error:
                return None
            if exact:
                return legacy_cache_row_to_lyrics(exact)
            if not track.duration_sec or track.duration_sec <= 0:
                return None
            try:
                rows = self.conn.execute(
                    "SELECT key, synced_lyrics, plain_lyrics, instrumental FROM lyrics_cache"
                ).fetchall()
            except sqlite3.Error:
                return None
        want_title = canonical_lyrics_cache_title(track.title)
        best: tuple[int, sqlite3.Row] | None = None
        for row in rows:
            parts = str(row["key"] or "").split("\x1f")
            if len(parts) != 3:
                continue
            title, _artist, duration_text = parts
            if canonical_lyrics_cache_title(title) != want_title:
                continue
            try:
                duration_delta = abs(int(duration_text) - int(track.duration_sec))
            except ValueError:
                continue
            if duration_delta > 2:
                continue
            if best is None or duration_delta < best[0]:
                best = (duration_delta, row)
                if duration_delta == 0:
                    break
        if best is None:
            return None
        return legacy_cache_row_to_lyrics(best[1])

    def save_lrclib(self, track: TrackInfo, lyrics: Lyrics) -> None:
        lrc = lyrics_to_storage_text(lyrics)
        with self.lock:
            self.conn.execute(
                """
                INSERT OR REPLACE INTO lyrics
                (video_id, source, synced, instrumental, lrc, title, artist, album, duration_sec, fetched_at)
                VALUES (?, 'lrclib', ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    track.key,
                    1 if lyrics.synced else 0,
                    1 if lyrics.instrumental else 0,
                    lrc,
                    track.title,
                    track.artist,
                    track.album,
                    int(track.duration_sec or 0),
                    int(time.time()),
                ),
            )
            self.conn.commit()

    def save_manual(self, track: TrackInfo, lyrics: Lyrics) -> None:
        lrc = lyrics_to_storage_text(lyrics)
        with self.lock:
            self.conn.execute(
                """
                INSERT OR REPLACE INTO lyrics
                (video_id, source, synced, instrumental, lrc, title, artist, album, duration_sec, fetched_at)
                VALUES (?, 'manual', ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    track.key,
                    1 if lyrics.synced else 0,
                    1 if lyrics.instrumental else 0,
                    lrc,
                    track.title,
                    track.artist,
                    track.album,
                    int(track.duration_sec or 0),
                    int(time.time()),
                ),
            )
            self.conn.commit()

    def clear_cache(self, source: str = "lrclib", video_id: str | None = None) -> int:
        if source not in {"lrclib", "manual"}:
            raise ValueError("source must be 'lrclib' or 'manual'")
        with self.lock:
            if video_id:
                cur = self.conn.execute("DELETE FROM lyrics WHERE source = ? AND video_id = ?", (source, video_id))
            else:
                cur = self.conn.execute("DELETE FROM lyrics WHERE source = ?", (source,))
            self.conn.commit()
            return cur.rowcount if cur.rowcount is not None else 0

    def stats(self) -> list[sqlite3.Row]:
        with self.lock:
            return list(
                self.conn.execute(
                    """
                    SELECT source,
                           COUNT(*) AS rows,
                           SUM(CASE WHEN lrc IS NULL THEN 1 ELSE 0 END) AS negative_rows
                    FROM lyrics
                    GROUP BY source
                    ORDER BY source
                    """
                )
            )


def row_to_lyrics(row: sqlite3.Row) -> Lyrics:
    if row["instrumental"]:
        return Lyrics(instrumental=True, resolved=True)
    text = row["lrc"] or ""
    if row["synced"]:
        synced, syllables = parse_lrc_with_syllables(text)
        return Lyrics(synced=synced, syllables=syllables, resolved=True)
    return Lyrics(plain=[line.strip() for line in text.splitlines() if line.strip()], resolved=True)


def normalized_lyrics_cache_part(value: str) -> str:
    return re.sub(r"\s+", " ", value or "").strip().lower()


LEGACY_CACHE_FEAT_RE = re.compile(
    r"\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with|prod\.?|ร่วมกับ)\b[^)\]]*[\)\]]?",
    re.IGNORECASE,
)
LEGACY_CACHE_TITLE_NOISE_RE = re.compile(
    r"\s*[\(\[]\s*[^)\]]*\b(?:remaster(?:ed)?|original\s+version|mono|stereo|"
    r"album\s+version|single\s+version)\b[^)\]]*[\)\]]"
    r"|\s*-\s*(?:\d{2,4}\s+)?(?:remaster(?:ed)?|original\s+version|mono|stereo)\b.*$",
    re.IGNORECASE,
)
LEGACY_CACHE_MATCH_PUNCT_RE = re.compile(r"[^\w\s]", re.UNICODE)


def legacy_lyrics_cache_key(track: TrackInfo) -> str:
    duration = int(track.duration_sec) if track.duration_sec and track.duration_sec > 0 else 0
    return "\x1f".join(
        (
            normalized_lyrics_cache_part(track.title),
            normalized_lyrics_cache_part(track.artist),
            str(duration),
        )
    )


def canonical_lyrics_cache_title(title: str) -> str:
    title = normalized_lyrics_cache_part(title)
    title = LEGACY_CACHE_FEAT_RE.sub(" ", title)
    title = LEGACY_CACHE_TITLE_NOISE_RE.sub(" ", title)
    title = LEGACY_CACHE_MATCH_PUNCT_RE.sub(" ", title)
    return normalized_lyrics_cache_part(title)


def legacy_cache_row_to_lyrics(row: sqlite3.Row) -> Lyrics:
    return lyrics_from_lrclib_payload(
        {
            "syncedLyrics": row["synced_lyrics"],
            "plainLyrics": row["plain_lyrics"],
            "instrumental": bool(row["instrumental"]),
        }
    )


def lyrics_to_storage_text(lyrics: Lyrics) -> str | None:
    if lyrics.instrumental:
        return ""
    if lyrics.synced:
        return "\n".join("[%d:%05.2f]%s" % (ms // 60000, (ms % 60000) / 1000.0, line) for ms, line in lyrics.synced)
    if lyrics.plain:
        return "\n".join(lyrics.plain)
    return None


def parse_lrc(text: str) -> list[tuple[int, str]]:
    synced, _syllables = parse_lrc_with_syllables(text)
    return synced


def parse_lrc_timestamp(minutes: str, seconds: str) -> int:
    return int((int(minutes) * 60 + float(seconds)) * 1000)


def strip_lrc_metadata(text: str) -> str:
    return re.sub(r"\[(?:ar|al|ti|by|offset|length|re):[^\]]*\]", "", text, flags=re.IGNORECASE)


def parse_lrc_with_syllables(text: str) -> tuple[list[tuple[int, str]], dict[int, list[tuple[int, str]]]]:
    lines: list[tuple[int, str]] = []
    syllables_by_line: dict[int, list[tuple[int, str]]] = {}
    for raw in (text or "").splitlines():
        raw = strip_lrc_metadata(raw)
        stamps = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", raw)
        if not stamps:
            continue
        body = re.sub(r"(?:\[\d+:\d+(?:\.\d+)?\])+", "", raw).strip()
        syllables = parse_inline_syllables(body)
        lyric = re.sub(r"<\d+:\d+(?:\.\d+)?>", "", body).strip()
        if not lyric:
            continue
        for minutes, seconds in stamps:
            try:
                line_ms = parse_lrc_timestamp(minutes, seconds)
                lines.append((line_ms, lyric))
                if syllables:
                    syllables_by_line[line_ms] = syllables
            except ValueError:
                pass
    return sorted(lines, key=lambda item: item[0]), syllables_by_line


def parse_inline_syllables(text: str) -> list[tuple[int, str]]:
    parts = list(re.finditer(r"<(\d+):(\d+(?:\.\d+)?)>", text or ""))
    if not parts:
        return []
    syllables: list[tuple[int, str]] = []
    for idx, match in enumerate(parts):
        start = match.end()
        end = parts[idx + 1].start() if idx + 1 < len(parts) else len(text)
        token = text[start:end].strip()
        if not token:
            continue
        try:
            syllables.append((parse_lrc_timestamp(match.group(1), match.group(2)), token))
        except ValueError:
            continue
    return syllables


def lyrics_from_lrclib_payload(payload: dict[str, Any] | None) -> Lyrics:
    if not payload:
        return Lyrics(resolved=True)
    if payload.get("instrumental"):
        return Lyrics(instrumental=True, resolved=True)
    synced, syllables = parse_lrc_with_syllables(payload.get("syncedLyrics") or "")
    plain = [line.strip() for line in (payload.get("plainLyrics") or "").splitlines() if line.strip()]
    return Lyrics(synced=synced, syllables=syllables, plain=plain, resolved=True)


class ResolveCancelled(Exception):
    """Raised when a track change abandons an in-flight lyrics fetch."""


def _is_timeout_error(exc: BaseException) -> bool:
    if isinstance(exc, (socket.timeout, TimeoutError)):
        return True
    if isinstance(exc, urllib.error.URLError):
        return _is_timeout_error(exc.reason) if isinstance(exc.reason, BaseException) else False
    return False


def _is_retryable_error(exc: BaseException) -> bool:
    if _is_timeout_error(exc):
        return True
    if isinstance(exc, urllib.error.HTTPError) and exc.code in LRCLIB_RETRYABLE_STATUS:
        return True
    return False


def _retry_delay_seconds(exc: BaseException) -> float:
    """Delay before the next attempt, honoring a Retry-After header if lrclib
    sends one (Cloudflare 503/429 sometimes does), capped so a track change
    doesn't wait absurdly long."""
    if isinstance(exc, urllib.error.HTTPError):
        header = exc.headers.get("Retry-After") if exc.headers else None
        if header:
            try:
                return max(0.0, min(float(header), LRCLIB_MAX_RETRY_AFTER_SECONDS))
            except ValueError:
                pass
    return LRCLIB_RETRY_DELAY_SECONDS


def lrclib_request(path: str, params: dict[str, Any], cancel: threading.Event | None = None) -> Any:
    url = "https://lrclib.net" + path + "?" + urllib.parse.urlencode(
        {k: v for k, v in params.items() if v not in ("", None, 0)}
    )
    req = urllib.request.Request(url, headers={"User-Agent": LRCLIB_USER_AGENT})
    for attempt in range(LRCLIB_RETRY_ATTEMPTS):
        # A blocking urlopen can't be interrupted, so cancellation takes effect
        # at request/retry boundaries -- at worst one in-flight request finishes.
        if cancel is not None and cancel.is_set():
            raise ResolveCancelled()
        try:
            with urllib.request.urlopen(req, timeout=LRCLIB_TIMEOUT_SECONDS) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as exc:
            if not _is_retryable_error(exc) or attempt + 1 >= LRCLIB_RETRY_ATTEMPTS:
                raise
            delay = _retry_delay_seconds(exc)
            if cancel is not None and cancel.wait(delay):
                raise ResolveCancelled()
            if cancel is None:
                time.sleep(delay)


def fetch_lrclib(track: TrackInfo, cancel: threading.Event | None = None) -> Lyrics:
    """Resolve lyrics from lrclib, accepting only synced (or instrumental) results.

    Plain-only lyrics are treated as not found: the display can't scroll them
    in time, so two static lines would sit on screen for the whole song.
    """
    params = {
        "track_name": track.title,
        "artist_name": track.artist,
        "album_name": track.album,
        "duration": int(track.duration_sec or 0),
    }
    try:
        exact = lrclib_request("/api/get", params, cancel)
        lyrics = lyrics_from_lrclib_payload(exact)
        if lyrics.synced or lyrics.instrumental:
            return lyrics
    except urllib.error.HTTPError as exc:
        # 404: no exact match. 400: /api/get demands track+artist+album+duration,
        # so tracks with a missing field can only be found via /api/search.
        if exc.code not in (400, 404):
            raise

    results = lrclib_request("/api/search", params, cancel)
    candidates = results if isinstance(results, list) else []
    duration = float(track.duration_sec or 0)
    best_synced: tuple[float, dict[str, Any]] | None = None
    for item in candidates:
        lyrics = lyrics_from_lrclib_payload(item)
        if not lyrics.synced:
            continue
        item_duration = float(item.get("duration") or 0)
        distance = abs(item_duration - duration) if duration > 0 and item_duration > 0 else 0
        if duration > 0 and distance > 3:
            continue
        if best_synced is None or distance < best_synced[0]:
            best_synced = (distance, item)
    return lyrics_from_lrclib_payload(best_synced[1] if best_synced else None)


class FrameRenderer:
    def render(self, state: AppState, profile: RenderProfile = DEFAULT_PROFILE) -> bytes:
        raise NotImplementedError


class CoreTextFrameRenderer(FrameRenderer):
    def __init__(self, font_name: str = "Tahoma") -> None:
        import CoreText  # type: ignore
        import Quartz  # type: ignore
        from Foundation import NSAttributedString, NSString  # type: ignore

        self.CoreText = CoreText
        self.Quartz = Quartz
        self.NSAttributedString = NSAttributedString
        self.NSString = NSString
        self.font_name = font_name
        self.color_space = Quartz.CGColorSpaceCreateDeviceGray()
        self.rgb_color_space = Quartz.CGColorSpaceCreateDeviceRGB()
        self.brand_fonts = self._load_brand_fonts()
        # Height of the frame currently being rendered; the Core Text primitives
        # need it to flip top-origin layout coords into Quartz's bottom-origin
        # space. render() sets it per call (rendering is single-threaded).
        self._h = DISPLAY_HEIGHT

    def _load_brand_fonts(self) -> dict[str, str]:
        """Resolve the Sukhumvit Set weight used for each Tend type role.

        Sukhumvit Set is a macOS system family, so there is nothing to register.
        CTFontCreateWithName silently substitutes a default face when a name
        does not resolve, so we confirm each weight actually maps to Sukhumvit
        (its PostScript names start with "SukhumvitSet") and otherwise fall back
        to self.font_name for that role.
        """
        ct = self.CoreText
        resolved: dict[str, str] = {}
        for role, name in BRAND_FONT_NAMES.items():
            font = ct.CTFontCreateWithName(name, 24.0, None)
            ps_name = str(ct.CTFontCopyPostScriptName(font))
            if ps_name.startswith("SukhumvitSet"):
                resolved[role] = name
            else:
                print(f"font {name!r} unavailable (resolved to {ps_name!r}); "
                      f"{role!r} falls back to {self.font_name!r}")
        return resolved

    def _role_font(self, role: str) -> str:
        return self.brand_fonts.get(role, self.font_name)

    def render(self, state: AppState, profile: RenderProfile = DEFAULT_PROFILE) -> bytes:
        q = self.Quartz
        self._h = profile.height
        ctx = q.CGBitmapContextCreate(
            None, profile.width, profile.height, 8, 0, self.color_space, q.kCGImageAlphaNone
        )
        q.CGContextSetGrayFillColor(ctx, 0.0, 1.0)
        q.CGContextFillRect(ctx, q.CGRectMake(0, 0, profile.width, profile.height))
        q.CGContextSetGrayFillColor(ctx, 1.0, 1.0)
        q.CGContextSetShouldAntialias(ctx, True)
        q.CGContextSetTextMatrix(ctx, q.CGAffineTransformIdentity)

        if profile.name == SQUARE_PROFILE.name:
            self._draw_square(ctx, state, profile)
        else:
            self._draw_wide(ctx, state, profile)

        image = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(image)))
        stride = q.CGImageGetBytesPerRow(image)
        return pack_1bpp(pixels, profile.width, profile.height, stride)

    def decode_cover(self, data: bytes, size: int) -> CoverArt | None:
        """Decode, center-crop and downscale cover art for mono and RGB565.

        Runs off the event loop (called via asyncio.to_thread). It only touches
        the immutable color space and local Core Graphics contexts, so it is safe
        alongside the main render.
        """
        q = self.Quartz
        from Foundation import NSData  # type: ignore

        ns_data = NSData.dataWithBytes_length_(data, len(data))
        src = q.CGImageSourceCreateWithData(ns_data, None)
        if src is None or q.CGImageSourceGetCount(src) < 1:
            return None
        image = q.CGImageSourceCreateImageAtIndex(src, 0, None)
        if image is None:
            return None
        iw = int(q.CGImageGetWidth(image))
        ih = int(q.CGImageGetHeight(image))
        if iw <= 0 or ih <= 0:
            return None
        side = min(iw, ih)
        crop = q.CGImageCreateWithImageInRect(
            image, q.CGRectMake((iw - side) // 2, (ih - side) // 2, side, side)
        )
        # A fixed RGBA byte order gives us stable channel positions across
        # architectures. The same decoded pixels feed the legacy 1-bit dither
        # and the ESP8266's true-color overlay.
        bitmap_info = q.kCGImageAlphaPremultipliedLast | q.kCGBitmapByteOrder32Big
        ctx = q.CGBitmapContextCreate(
            None, size, size, 8, size * 4, self.rgb_color_space, bitmap_info
        )
        if ctx is None:
            return None
        q.CGContextSetInterpolationQuality(ctx, q.kCGInterpolationHigh)
        q.CGContextDrawImage(ctx, q.CGRectMake(0, 0, size, size), crop or image)
        out = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(out)))
        stride = int(q.CGImageGetBytesPerRow(out))
        # Core Graphics bitmap memory is top-origin (row 0 = top), matching the
        # frame buffer, so no vertical flip is needed here.
        gray: list[int] = []
        rgb565 = bytearray(size * size * 2)
        dst = 0
        for y in range(size):
            row = y * stride
            for x in range(size):
                src_px = row + x * 4
                r, g, b = pixels[src_px], pixels[src_px + 1], pixels[src_px + 2]
                gray.append((77 * r + 150 * g + 29 * b) >> 8)
                color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                rgb565[dst] = color >> 8
                rgb565[dst + 1] = color & 0xFF
                dst += 2
        return CoverArt(
            size=size,
            bits=floyd_steinberg_1bit(gray, size, size),
            rgb565=bytes(rgb565),
        )

    def render_pause_chip(self, profile: RenderProfile) -> tuple[bytes, bytes, int, int]:
        """Render the small PAUSED chip stamped over paused full-screen art.

        Returns (value, mask, chip_w, chip_h): two 1-bpp row-packed bitmaps of
        the same size. ``value`` is the white pill with black label; ``mask`` is
        the pill's coverage (lit = inside pill). The daemon composites them over
        the album art so the pill's rounded corners keep the photo behind them.
        """
        ct = self.CoreText
        font_name = self._role_font("label")
        if profile.name == SQUARE_PROFILE.name:
            size, pad_x, chip_h, rise = 11, 7, 16, 11
        else:
            size, pad_x, chip_h, rise = 13, 9, 20, 14
        text = "PAUSED"
        font = ct.CTFontCreateWithName(font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
            ct.kCTKernAttributeName: TRACKING_MEGA * size,
        }
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        _left, text_w, _advance = self._line_visual_metrics(line)
        chip_w = int(text_w) + pad_x * 2
        value = self._render_chip_layer(text, size, pad_x, rise, chip_w, chip_h, font_name, True)
        mask = self._render_chip_layer(text, size, pad_x, rise, chip_w, chip_h, font_name, False)
        return value, mask, chip_w, chip_h

    def _render_chip_layer(
        self,
        text: str,
        size: int,
        pad_x: int,
        rise: int,
        chip_w: int,
        chip_h: int,
        font_name: str,
        with_text: bool,
    ) -> bytes:
        q = self.Quartz
        ctx = q.CGBitmapContextCreate(
            None, chip_w, chip_h, 8, 0, self.color_space, q.kCGImageAlphaNone
        )
        q.CGContextSetGrayFillColor(ctx, 0.0, 1.0)
        q.CGContextFillRect(ctx, q.CGRectMake(0, 0, chip_w, chip_h))
        q.CGContextSetShouldAntialias(ctx, True)
        q.CGContextSetTextMatrix(ctx, q.CGAffineTransformIdentity)
        prev_h = self._h
        self._h = chip_h
        try:
            q.CGContextSetGrayFillColor(ctx, 1.0, 1.0)
            q.CGContextAddPath(ctx, self._pill_path(0, 0, chip_w, chip_h))
            q.CGContextFillPath(ctx)
            if with_text:
                self._draw_text(
                    ctx, text, size, pad_x, rise, chip_w, "left",
                    font_name=font_name, tracking=TRACKING_MEGA, gray=0.0,
                )
        finally:
            self._h = prev_h
        image = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(image)))
        stride = q.CGImageGetBytesPerRow(image)
        return pack_1bpp(pixels, chip_w, chip_h, stride)

    def _draw_lyric_band(self, ctx: Any, state: AppState, profile: RenderProfile) -> LyricLayout:
        lyric_font = self._role_font("lyric")
        current, _next_line, _third = state.current_lines()
        current_layout = fit_lyric_layout(
            current,
            profile.lyric_box_width,
            profile.lyric_band_bottom_y - profile.lyric_sizes.band_top_y,
            profile.lyric_sizes,
            self._break_text,
        )
        # Karaoke fill only makes sense for timed lines; loading/error/plain and
        # "Instrumental" copy would otherwise render fully hollow at fraction 0.
        karaoke = bool(state.lyrics.synced) and not state.lyrics.instrumental
        fraction = state.current_line_highlight_fraction() if karaoke else 1.0
        total_chars = sum(len(row) for row in current_layout.rows) or 1
        consumed_chars = 0
        for row, baseline_y in zip(current_layout.rows, current_layout.baselines):
            row_fraction = clamp(
                (fraction * total_chars - consumed_chars) / max(1, len(row)), 0.0, 1.0
            )
            self._draw_karaoke_text(
                ctx,
                row,
                current_layout.font_size,
                profile.lyric_box_x,
                baseline_y,
                profile.lyric_box_width,
                row_fraction if karaoke else 1.0,
                lyric_font,
            )
            consumed_chars += len(row)
        return current_layout

    def _draw_wide(self, ctx: Any, state: AppState, profile: RenderProfile) -> None:
        label_font = self._role_font("label")
        display_font = self._role_font("display")
        body_font = self._role_font("body")
        body_medium_font = self._role_font("body_medium")

        self._draw_text(
            ctx, "NOW PLAYING", 13, 14, 18, DISPLAY_WIDTH - 28, "left", font_name=label_font, tracking=TRACKING_MEGA
        )
        self._draw_rule(ctx, 12, 28, DISPLAY_WIDTH - 24)

        if not state.track.title:
            # Idle: quiet centered copy in the design system's lowercase voice.
            self._draw_text(ctx, "nothing playing", 30, 14, 150, DISPLAY_WIDTH - 28, "center", font_name=display_font)
            self._draw_text(
                ctx, "waiting for youtube music", 15, 14, 182, DISPLAY_WIDTH - 28, "center", font_name=body_font
            )
            self._screen_rect(ctx, 14, 168, DISPLAY_WIDTH - 28, 20)
            self._draw_text(
                ctx, "POWERED BY CLAUDE", 13, 260, 292, 126, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
            )
            return

        place = cover_placement(profile) if state.cover is not None else None
        header_w = place.title_w if place is not None else DISPLAY_WIDTH - 28
        self._draw_text(ctx, state.track.title, 28, 14, 60, header_w, "left", font_name=display_font)
        self._draw_text(ctx, state.track.artist, 18, 14, 88, header_w, "left", font_name=body_medium_font)
        if place is not None:
            self._draw_cover_card(ctx, place)
        self._draw_progress(ctx, state, MONO_FONT_NAME, WIDE_PROGRESS_GEOM)
        self._draw_rule(ctx, 12, 130, DISPLAY_WIDTH - 24)

        _current, next_line, third = state.current_lines()
        current_layout = self._draw_lyric_band(ctx, state, profile)
        self._draw_text(ctx, next_line, 21, 14, 224, DISPLAY_WIDTH - 28, "center", font_name=body_font)
        if not current_layout.hide_third and third:
            self._draw_text(ctx, third, 16, 14, 252, DISPLAY_WIDTH - 28, "center", font_name=body_font)
            self._screen_rect(ctx, 14, 238, DISPLAY_WIDTH - 28, 22)
        if state.clock.paused:
            self._draw_state_chip(ctx, "PAUSED", 14, 292, font_name=label_font)
        else:
            self._draw_text(
                ctx, "PLAYING", 13, 14, 292, 120, "left", font_name=label_font, tracking=TRACKING_MEGA
            )
        self._draw_text(
            ctx, "1.0.0", 13, 260, 292, 126, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
        )

    def _draw_square(self, ctx: Any, state: AppState, profile: RenderProfile) -> None:
        """Compact 240x240 layout for the ESP8266 SmallTV boards.

        Same structure as the wide layout -- eyebrow, title/artist, progress,
        karaoke lyric band, next line, footer -- just tighter type scale.
        """
        width = profile.width
        label_font = self._role_font("label")
        display_font = self._role_font("display")
        body_font = self._role_font("body")
        body_medium_font = self._role_font("body_medium")

        self._draw_text(
            ctx, "NOW PLAYING", 11, 12, 14, width - 24, "left", font_name=label_font, tracking=TRACKING_MEGA
        )
        self._draw_rule(ctx, 10, 24, width - 20)

        if not state.track.title:
            self._draw_text(ctx, "nothing playing", 22, 12, 112, width - 24, "center", font_name=display_font)
            self._draw_text(
                ctx, "waiting for youtube music", 13, 12, 140, width - 24, "center", font_name=body_font
            )
            self._screen_rect(ctx, 12, 126, width - 24, 18)
            self._draw_text(
                ctx, "1.0.0", 11, 116, 228, width - 128, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
            )
            return

        place = cover_placement(profile) if state.cover is not None else None
        header_w = place.title_w if place is not None else width - 24
        self._draw_text(ctx, state.track.title, 20, 12, 52, header_w, "left", font_name=display_font)
        self._draw_text(ctx, state.track.artist, 14, 12, 74, header_w, "left", font_name=body_medium_font)
        if place is not None:
            self._draw_cover_card(ctx, place)
        self._draw_progress(ctx, state, MONO_FONT_NAME, SQUARE_PROGRESS_GEOM)
        self._draw_rule(ctx, 10, 104, width - 20)

        _current, next_line, third = state.current_lines()
        current_layout = self._draw_lyric_band(ctx, state, profile)
        self._draw_text(ctx, next_line, 14, 12, 202, width - 24, "center", font_name=body_font)
        if not current_layout.hide_third and third:
            self._draw_text(ctx, third, 12, 12, 222, width - 24, "center", font_name=body_font)
            self._screen_rect(ctx, 12, 210, width - 24, 16)
        if state.clock.paused:
            self._draw_state_chip(
                ctx, "PAUSED", 12, 234, font_name=label_font, size=11, pad_x=7, chip_h=16, rise=11
            )
        else:
            self._draw_text(
                ctx, "PLAYING", 11, 12, 234, 110, "left", font_name=label_font, tracking=TRACKING_MEGA
            )
        self._draw_text(
            ctx, "1.0.0", 11, 116, 234, width - 128, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
        )

    def _break_text(self, text: str, size: float, width: int) -> list[str]:
        if not text:
            return []
        ct = self.CoreText
        font = ct.CTFontCreateWithName(self._role_font("display"), size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        ns_text = self.NSString.stringWithString_(text)
        attributed = self.NSAttributedString.alloc().initWithString_attributes_(ns_text, attrs)
        typesetter = ct.CTTypesetterCreateWithAttributedString(attributed)
        rows: list[str] = []
        offset = 0
        text_len = int(ns_text.length())
        while offset < text_len:
            count = int(ct.CTTypesetterSuggestLineBreak(typesetter, offset, float(width)))
            if count <= 0:
                count = 1
            end = min(text_len, offset + count)
            rows.append(str(ns_text.substringWithRange_((offset, end - offset))))
            offset = end
            while offset < text_len:
                ch = ns_text.characterAtIndex_(offset)
                if not (ch if isinstance(ch, str) else chr(ch)).isspace():
                    break
                offset += 1
        return rows

    def _draw_text(
        self,
        ctx: Any,
        text: str,
        size: float,
        x: int,
        baseline_y_top_origin: int,
        width: int,
        align: str,
        font_name: str | None = None,
        tracking: float = 0.0,
        gray: float = 1.0,
    ) -> None:
        if not text:
            return
        ct = self.CoreText
        q = self.Quartz
        font = ct.CTFontCreateWithName(font_name or self.font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        if tracking:
            attrs[ct.kCTKernAttributeName] = tracking * size
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        measured = ct.CTLineGetTypographicBounds(line, None, None, None)
        text_width = measured[0] if isinstance(measured, tuple) else measured
        probe = q.CGBitmapContextCreate(None, 1, 1, 8, 1, self.color_space, q.kCGImageAlphaNone)
        q.CGContextSetTextMatrix(probe, q.CGAffineTransformIdentity)
        ink = ct.CTLineGetImageBounds(line, probe)
        ink_x = ink.origin.x
        ink_y = ink.origin.y
        ink_w = ink.size.width
        ink_h = ink.size.height
        if not (ink_w > 0 and ink_h > 0):
            ascent = ct.CTFontGetAscent(font)
            descent = ct.CTFontGetDescent(font)
            ink_x = 0.0
            ink_y = -descent
            ink_w = text_width
            ink_h = ascent + descent

        visual_left = min(0.0, ink_x)
        visual_right = max(float(text_width), ink_x + ink_w)
        visual_width = max(0.0, visual_right - visual_left)
        if align == "center":
            visual_x = x + max(0, int((width - visual_width) / 2))
        elif align == "right":
            visual_x = x + max(0, int(width - visual_width))
        else:
            visual_x = x
        draw_x = visual_x - visual_left

        baseline_y = self._h - baseline_y_top_origin
        pad = 3
        clip_y = max(0, math.floor(baseline_y + ink_y - pad))
        clip_top = min(self._h, math.ceil(baseline_y + ink_y + ink_h + pad))
        if clip_top <= clip_y:
            return

        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayFillColor(ctx, gray, 1.0)
        q.CGContextClipToRect(ctx, q.CGRectMake(x, clip_y, width, clip_top - clip_y))
        q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
        ct.CTLineDraw(line, ctx)
        q.CGContextRestoreGState(ctx)

    def _draw_rule(self, ctx: Any, x: int, y_top_origin: int, width: int) -> None:
        q = self.Quartz
        y = self._h - y_top_origin
        q.CGContextFillRect(ctx, q.CGRectMake(x, y, width, 1))

    def _pill_path(self, x: float, y: float, w: float, h: float) -> Any:
        q = self.Quartz
        radius = min(h, w) / 2.0
        return q.CGPathCreateWithRoundedRect(q.CGRectMake(x, y, w, h), radius, radius, None)

    def _fill_pill(self, ctx: Any, x: float, y: float, w: float, h: float) -> None:
        if w <= 0 or h <= 0:
            return
        q = self.Quartz
        q.CGContextAddPath(ctx, self._pill_path(x, y, w, h))
        q.CGContextFillPath(ctx)

    def _stroke_pill(self, ctx: Any, x: float, y: float, w: float, h: float) -> None:
        if w <= 0 or h <= 0:
            return
        q = self.Quartz
        q.CGContextAddPath(ctx, self._pill_path(x, y, w, h))
        q.CGContextStrokePath(ctx)

    def _draw_cover_card(self, ctx: Any, place: "CoverPlacement") -> None:
        """Tend hairline card frame around the album art (radius-8 rounded rect).

        Drawn in the pre-invert pass as set bits, so after the light-theme flip
        it reads as a 1px ink hairline on paper -- the same card chrome the other
        Tend screens use (u8g2 DrawRFrame, radius 8). The art is blitted inside
        this frame later, in _render, so it keeps its natural tonality.
        """
        q = self.Quartz
        pad = place.frame_pad
        fx = place.x - pad
        fw = place.size + 2 * pad
        fy_top = place.y - pad
        rect = q.CGRectMake(fx, self._h - fy_top - fw, fw, fw)
        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        q.CGContextAddPath(ctx, q.CGPathCreateWithRoundedRect(rect, 8.0, 8.0, None))
        q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)

    def _line_visual_metrics(self, line: Any) -> tuple[float, float, float]:
        """Return (visual_left, visual_width, advance_width) for a CTLine.

        visual_left/width mirror the union of advance width and image bounds
        that _draw_text uses for alignment.
        """
        ct = self.CoreText
        q = self.Quartz
        measured = ct.CTLineGetTypographicBounds(line, None, None, None)
        text_width = measured[0] if isinstance(measured, tuple) else measured
        probe = q.CGBitmapContextCreate(None, 1, 1, 8, 1, self.color_space, q.kCGImageAlphaNone)
        q.CGContextSetTextMatrix(probe, q.CGAffineTransformIdentity)
        ink = ct.CTLineGetImageBounds(line, probe)
        ink_x, ink_w = ink.origin.x, ink.size.width
        if not (ink_w > 0):
            ink_x, ink_w = 0.0, float(text_width)
        visual_left = min(0.0, ink_x)
        visual_right = max(float(text_width), ink_x + ink_w)
        return visual_left, max(0.0, visual_right - visual_left), float(text_width)

    def _draw_karaoke_text(
        self,
        ctx: Any,
        text: str,
        size: float,
        x: int,
        baseline_y_top_origin: int,
        width: int,
        fraction: float,
        font_name: str,
    ) -> None:
        """Centered lyric row: sung portion solid, unsung portion hollow outline."""
        if not text:
            return
        ct = self.CoreText
        q = self.Quartz
        font = ct.CTFontCreateWithName(font_name, size, None)
        solid_attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        # Stroke-only text ignores the context fill color; without an explicit
        # CGColor stroke attribute Core Text draws nothing here.
        hollow_attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTStrokeWidthAttributeName: max(KARAOKE_STROKE_PCT, 100.0 / size),
            ct.kCTStrokeColorAttributeName: q.CGColorCreateGenericGray(1.0, 1.0),
        }
        solid_line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, solid_attrs)
        )
        hollow_line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, hollow_attrs)
        )
        visual_left, visual_width, _advance = self._line_visual_metrics(solid_line)
        visual_x = x + max(0, int((width - visual_width) / 2))
        draw_x = visual_x - visual_left
        baseline_y = self._h - baseline_y_top_origin
        # The clip only bounds the horizontal wipe; keep it tall so Thai mark
        # stacks and deep descenders never get shaved (only this row's glyphs
        # are drawn in this pass, so a generous band cannot bleed).
        band_h = size * 2.4
        band_y = baseline_y - size * 0.8

        fraction = clamp(fraction, 0.0, 1.0)
        split_x = visual_x + visual_width * fraction

        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        if fraction < 1.0:
            q.CGContextSaveGState(ctx)
            q.CGContextClipToRect(ctx, q.CGRectMake(split_x, band_y, x + width - split_x, band_h))
            q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
            ct.CTLineDraw(hollow_line, ctx)
            q.CGContextRestoreGState(ctx)
        if fraction > 0.0:
            q.CGContextSaveGState(ctx)
            q.CGContextClipToRect(ctx, q.CGRectMake(x, band_y, split_x - x, band_h))
            q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
            ct.CTLineDraw(solid_line, ctx)
            q.CGContextRestoreGState(ctx)
        q.CGContextRestoreGState(ctx)

    def _screen_rect(self, ctx: Any, x: int, y_top_origin: int, w: int, h: int) -> None:
        """Punch a 50% checkerboard of background over a band.

        On the 1-bit panel this halftone-screens whatever was drawn there, so
        the text underneath reads as gray -- the design system's ink-muted.
        """
        q = self.Quartz
        y0 = self._h - y_top_origin - h
        q.CGContextSaveGState(ctx)
        q.CGContextSetShouldAntialias(ctx, False)
        q.CGContextSetGrayStrokeColor(ctx, 0.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        for row in range(h):
            q.CGContextSetLineDash(ctx, float((row + x) % 2), [1.0, 1.0], 2)
            q.CGContextBeginPath(ctx)
            q.CGContextMoveToPoint(ctx, float(x), y0 + row + 0.5)
            q.CGContextAddLineToPoint(ctx, float(x + w), y0 + row + 0.5)
            q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)

    def _draw_state_chip(
        self,
        ctx: Any,
        text: str,
        x: int,
        baseline_y_top_origin: int,
        font_name: str,
        size: int = 13,
        pad_x: int = 9,
        chip_h: int = 20,
        rise: int = 14,
    ) -> None:
        """Inverted pill chip -- the 1-bit stand-in for the ember accent."""
        ct = self.CoreText
        font = ct.CTFontCreateWithName(font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
            ct.kCTKernAttributeName: TRACKING_MEGA * size,
        }
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        _left, text_w, _advance = self._line_visual_metrics(line)
        chip_w = int(text_w) + pad_x * 2
        chip_y_top = baseline_y_top_origin - rise
        self._fill_pill(ctx, x, self._h - chip_y_top - chip_h, chip_w, chip_h)
        self._draw_text(
            ctx, text, size, x + pad_x, baseline_y_top_origin, chip_w, "left",
            font_name=font_name, tracking=TRACKING_MEGA, gray=0.0,
        )

    def _draw_progress(self, ctx: Any, state: AppState, font_name: str, geom: ProgressGeom) -> None:
        q = self.Quartz
        elapsed = state.clock.interpolated_position()
        duration = max(0.0, state.track.duration_sec)
        self._draw_text(
            ctx, format_time(elapsed), geom.time_size, geom.left_x, geom.baseline_y, geom.left_w,
            "left", font_name=font_name,
        )
        remaining = max(0.0, duration - elapsed) if duration else 0.0
        self._draw_text(
            ctx, "-" + format_time(remaining), geom.time_size, geom.right_x, geom.baseline_y, geom.right_w,
            "right", font_name=font_name,
        )
        bar_x, bar_w = geom.bar_x, geom.bar_w
        center_y = self._h - geom.center_y  # track centerline, bottom-up coords
        fraction = clamp(elapsed / duration, 0.0, 1.0) if duration > 0 else 0.0
        played_w = bar_w * fraction
        # remaining track: dotted hairline reads as ink-muted on the 1-bit panel
        q.CGContextSaveGState(ctx)
        q.CGContextSetShouldAntialias(ctx, False)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        q.CGContextSetLineDash(ctx, 0.0, [1.0, 3.0], 2)
        q.CGContextBeginPath(ctx)
        q.CGContextMoveToPoint(ctx, bar_x + played_w, center_y - 0.5)
        q.CGContextAddLineToPoint(ctx, bar_x + bar_w, center_y - 0.5)
        q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)
        # played track: solid bar, then the playhead dot on top
        if played_w > 0:
            self._fill_pill(ctx, bar_x, center_y - 1.5, played_w, 3)
        dot_r = geom.dot_r
        q.CGContextFillEllipseInRect(
            ctx, q.CGRectMake(bar_x + played_w - dot_r, center_y - dot_r, dot_r * 2, dot_r * 2)
        )


class FallbackFrameRenderer(FrameRenderer):
    def render(self, state: AppState, profile: RenderProfile = DEFAULT_PROFILE) -> bytes:
        width, height = profile.width, profile.height
        frame = bytearray(profile.frame_bytes)
        elapsed = state.clock.interpolated_position()
        duration = max(1.0, state.track.duration_sec or 1.0)
        geom = SQUARE_PROGRESS_GEOM if profile.name == SQUARE_PROFILE.name else WIDE_PROGRESS_GEOM
        fill = int(clamp(elapsed / duration, 0.0, 1.0) * geom.bar_w)
        draw_rect(frame, geom.bar_x, geom.center_y - 5, geom.bar_w, 10, width, height)
        if fill:
            fill_rect(frame, geom.bar_x + 1, geom.center_y - 4, fill, 8, width, height)
        fill_rect(frame, 12, 27 * height // DISPLAY_HEIGHT, width - 24, 1, width, height)
        fill_rect(frame, 12, 130 * height // DISPLAY_HEIGHT, width - 24, 1, width, height)
        return bytes(frame)


def format_time(seconds: float) -> str:
    seconds = max(0, int(seconds))
    return "%d:%02d" % (seconds // 60, seconds % 60)


def invert_frame(frame: bytes) -> bytes:
    return bytes(b ^ 0xFF for b in frame)


def pack_1bpp(pixels: bytes, width: int, height: int, stride: int) -> bytes:
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)
    for y in range(height):
        src_row = y * stride
        dst_row = y * row_bytes
        for x in range(width):
            if pixels[src_row + x] > 32:
                out[dst_row + (x // 8)] |= 1 << (x & 7)
    return bytes(out)


def set_pixel(frame: bytearray, x: int, y: int, width: int = DISPLAY_WIDTH, height: int = DISPLAY_HEIGHT) -> None:
    if 0 <= x < width and 0 <= y < height:
        row_bytes = width // 8
        frame[y * row_bytes + x // 8] |= 1 << (x & 7)


def fill_rect(
    frame: bytearray, x: int, y: int, w: int, h: int, width: int = DISPLAY_WIDTH, height: int = DISPLAY_HEIGHT
) -> None:
    for py in range(y, y + h):
        for px in range(x, x + w):
            set_pixel(frame, px, py, width, height)


def draw_rect(
    frame: bytearray, x: int, y: int, w: int, h: int, width: int = DISPLAY_WIDTH, height: int = DISPLAY_HEIGHT
) -> None:
    fill_rect(frame, x, y, w, 1, width, height)
    fill_rect(frame, x, y + h - 1, w, 1, width, height)
    fill_rect(frame, x, y, 1, h, width, height)
    fill_rect(frame, x + w - 1, y, 1, h, width, height)


class WebSocketConnection:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.reader = reader
        self.writer = writer
        self.write_lock = asyncio.Lock()
        self.board_frame_base: bytes | None = None
        self.board_color_key: tuple[int, int, int, int, bytes] | None = None
        self.profile: RenderProfile = DEFAULT_PROFILE
        self.supports_rgb565 = False
        self.tx_key: bytes | None = None
        self.rx_key: bytes | None = None
        self.tx_sequence = 0
        self.rx_sequence = 0
        self.outbound: asyncio.Queue[tuple[bytes, int]] | None = None
        self.sender_task: asyncio.Task[None] | None = None
        self.backpressure_events = 0
        self.pong_event = asyncio.Event()
        self.closed = False

    async def handshake(self, accept: Callable[[str, dict[str, str]], bool] | None = None) -> str:
        raw = await self.reader.readuntil(b"\r\n\r\n")
        head = raw.decode("iso-8859-1")
        lines = head.split("\r\n")
        request = lines[0].split()
        path = request[1] if len(request) > 1 else "/"
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.lower()] = value.strip()
        key = headers.get("sec-websocket-key")
        if not key:
            raise ValueError("missing Sec-WebSocket-Key")
        if accept is not None and not accept(path, headers):
            self.writer.write(b"HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            await self.writer.drain()
            raise ValueError("websocket handshake rejected")
        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        self.writer.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode("ascii")
        )
        await self.writer.drain()
        return path

    async def recv(self) -> tuple[int, bytes] | None:
        fragment_opcode: int | None = None
        fragments: list[bytes] = []
        while True:
            try:
                fin, opcode, payload = await self._read_frame()
            except asyncio.IncompleteReadError:
                return None
            if opcode == 8:
                await self.send(payload, opcode=8)
                return None
            if opcode == 9:
                await self.send(payload, opcode=10)
                continue
            if opcode == 10:
                self.pong_event.set()
                continue
            if opcode in (1, 2):
                if fragment_opcode is not None:
                    raise ValueError("new data frame before fragmented message completed")
                if fin:
                    return opcode, payload
                fragment_opcode = opcode
                fragments = [payload]
                continue
            if opcode == 0:
                if fragment_opcode is None:
                    raise ValueError("unexpected continuation frame")
                fragments.append(payload)
                if fin:
                    complete_opcode = fragment_opcode
                    complete_payload = b"".join(fragments)
                    fragment_opcode = None
                    fragments = []
                    return complete_opcode, complete_payload
                continue
            raise ValueError(f"unsupported websocket opcode {opcode}")

    async def _read_frame(self) -> tuple[bool, int, bytes]:
        first = await self.reader.readexactly(2)
        fin = bool(first[0] & 0x80)
        opcode = first[0] & 0x0F
        masked = bool(first[1] & 0x80)
        length = first[1] & 0x7F
        if not masked:
            raise ValueError("unmasked client websocket frame")
        if length == 126:
            length = struct.unpack("!H", await self.reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", await self.reader.readexactly(8))[0]
        if opcode in (8, 9, 10) and (not fin or length > 125):
            raise ValueError("invalid websocket control frame")
        mask = await self.reader.readexactly(4)
        payload = bytearray(await self.reader.readexactly(length))
        for i in range(length):
            payload[i] ^= mask[i % 4]
        return fin, opcode, bytes(payload)

    async def send_text(self, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if self.tx_key is not None:
            await self.send(self._protect(raw, SEC2_TEXT), opcode=2)
        else:
            await self.send(raw, opcode=1)

    async def send_binary(self, payload: bytes) -> None:
        await self.send(self._protect(payload, SEC2_BINARY) if self.tx_key is not None else payload, opcode=2)

    def enable_security(self, tx_key: bytes, rx_key: bytes) -> None:
        self.tx_key = tx_key
        self.rx_key = rx_key
        self.tx_sequence = 0
        self.rx_sequence = 0

    def _protect(self, payload: bytes, content_type: int) -> bytes:
        if self.tx_key is None:
            raise ValueError("secure session is not initialized")
        self.tx_sequence += 1
        header = SEC2_HEADER_STRUCT.pack(SEC2_MAGIC, SEC2_VERSION, content_type, 0, self.tx_sequence, len(payload))
        return header + payload + hmac.new(self.tx_key, header + payload, hashlib.sha256).digest()

    def _unprotect(self, record: bytes) -> tuple[int, bytes]:
        if self.rx_key is None or len(record) < SEC2_HEADER_STRUCT.size + SEC2_TAG_BYTES:
            raise ValueError("invalid SEC2 record")
        header = record[: SEC2_HEADER_STRUCT.size]
        magic, version, content_type, reserved, sequence, payload_len = SEC2_HEADER_STRUCT.unpack(header)
        expected_len = SEC2_HEADER_STRUCT.size + payload_len + SEC2_TAG_BYTES
        if magic != SEC2_MAGIC or version != SEC2_VERSION or reserved != 0 or expected_len != len(record):
            raise ValueError("invalid SEC2 header")
        if sequence != self.rx_sequence + 1:
            raise ValueError("SEC2 replay or sequence gap")
        payload = record[SEC2_HEADER_STRUCT.size : -SEC2_TAG_BYTES]
        tag = record[-SEC2_TAG_BYTES:]
        expected = hmac.new(self.rx_key, header + payload, hashlib.sha256).digest()
        if not hmac.compare_digest(tag, expected):
            raise ValueError("SEC2 authentication failed")
        if content_type not in (SEC2_TEXT, SEC2_BINARY):
            raise ValueError("invalid SEC2 content type")
        self.rx_sequence = sequence
        return content_type, payload

    async def recv_application(self) -> tuple[int, bytes] | None:
        message = await self.recv()
        if message is None:
            return None
        opcode, payload = message
        if self.rx_key is None:
            return message
        if opcode != 2:
            raise ValueError("secure application data must use binary WebSocket frames")
        content_type, clear = self._unprotect(payload)
        return (1 if content_type == SEC2_TEXT else 2), clear

    def start_sender(self) -> None:
        if self.outbound is not None:
            return
        self.outbound = asyncio.Queue(maxsize=BOARD_QUEUE_DEPTH)
        self.sender_task = asyncio.create_task(self._sender_loop())

    async def _sender_loop(self) -> None:
        assert self.outbound is not None
        try:
            while True:
                payload, opcode = await self.outbound.get()
                async with self.write_lock:
                    self._write_frame(payload, opcode)
                    await self.writer.drain()
                self.backpressure_events = 0
        except (asyncio.CancelledError, OSError, ConnectionError):
            pass
        finally:
            self.closed = True
            self.writer.close()

    async def send(self, payload: bytes, opcode: int) -> None:
        if self.closed:
            raise ConnectionError("board connection is closed")
        if self.outbound is not None:
            try:
                self.outbound.put_nowait((payload, opcode))
                return
            except asyncio.QueueFull:
                self.backpressure_events += 1
                # A full bounded queue means this board has already remained
                # behind for several complete messages. SEC2 sequences cannot
                # safely skip a dropped record, so isolate it immediately.
                self.close()
                raise ConnectionError("board outbound queue is full")
        async with self.write_lock:
            self._write_frame(payload, opcode)
            await self.writer.drain()

    def _write_frame(self, payload: bytes, opcode: int) -> None:
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length < 65536:
            header.extend((126, *struct.pack("!H", length)))
        else:
            header.extend((127, *struct.pack("!Q", length)))
        self.writer.write(bytes(header) + payload)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.sender_task is not None:
            self.sender_task.cancel()
        self.writer.close()

    async def heartbeat(self) -> None:
        while not self.closed:
            await asyncio.sleep(HEARTBEAT_INTERVAL_SECONDS)
            self.pong_event.clear()
            await self.send(str(monotonic_ms()).encode("ascii"), opcode=9)
            try:
                await asyncio.wait_for(self.pong_event.wait(), HEARTBEAT_TIMEOUT_SECONDS)
            except asyncio.TimeoutError:
                self.close()
                raise ConnectionError("board heartbeat timed out")


class LyricsDisplayDaemon:
    def __init__(self, store: LyricsStore, renderer: FrameRenderer, board_token: str = "",
                 identity: DaemonIdentity | None = None, allow_legacy_proto1: bool = False) -> None:
        self.store = store
        self.renderer = renderer
        self.board_token = board_token
        self.identity = identity
        self.allow_legacy_proto1 = allow_legacy_proto1
        self.state = AppState()
        self.theme = "light"
        self.state_lock = asyncio.Lock()
        self.boards: set[WebSocketConnection] = set()
        self.last_frames: dict[str, bytes] = {}  # profile name -> latest now-frame
        self.schedule_generation = 0
        self.scheduled_task: asyncio.Task[None] | None = None
        self._background_tasks: set[asyncio.Task[None]] = set()
        self._resolve_cancel: threading.Event | None = None
        self.media_timeline_offset_sec = 0.0
        # Album art dithered to 1-bit, cached per (cover URL, size) (bounded).
        self.cover_cache: dict[tuple[str, int], CoverArt | None] = {}

    def _cancel_inflight_resolve(self) -> None:
        if self._resolve_cancel is not None:
            self._resolve_cancel.set()
            self._resolve_cancel = None

    async def handle_extension(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        conn = WebSocketConnection(reader, writer)
        try:
            path = await conn.handshake()
            if path != "/extension":
                conn.close()
                return
            print("extension connected")
            while True:
                message = await conn.recv()
                if message is None:
                    break
                opcode, payload = message
                if opcode != 1:
                    continue
                await self.handle_extension_message(payload.decode("utf-8"))
        except (OSError, ValueError, ConnectionError, asyncio.IncompleteReadError,
                asyncio.LimitOverrunError) as exc:
            # Same probe/half-open handling as handle_board: the handshake read
            # can EOF or overrun before a request arrives -- log, don't crash.
            print(f"extension websocket closed: {exc}")
        print("extension disconnected")

    async def handle_extension_message(self, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        msg_type = msg.get("type")
        payload = msg.get("payload") or {}
        if msg_type == "now-playing":
            track = TrackInfo(
                video_id=str(payload.get("videoId") or ""),
                title=str(payload.get("title") or ""),
                artist=str(payload.get("artist") or ""),
                album=str(payload.get("album") or ""),
                duration_sec=float(payload.get("durationSec") or 0),
                art_url=str(payload.get("artUrl") or ""),
            )
            await self.set_track(track)
            # Newer extensions include the current media time with the track so
            # reconnecting halfway through a song cannot flash the start intro.
            # Keep accepting the old track-only payload for compatibility.
            if any(field in payload for field in ("positionSec", "paused", "playbackRate")):
                await self.update_clock(payload)
        elif msg_type == "tick":
            await self.update_clock(payload)
        elif msg_type == "event":
            event_type = payload.get("type")
            await self.update_clock(payload, update_duration=event_type != "ended")
            if event_type == "ended":
                async with self.state_lock:
                    self._cancel_inflight_resolve()
                    if self.state.track.duration_sec > 0:
                        self.media_timeline_offset_sec += self.state.track.duration_sec
                    self.state.track = TrackInfo()
                    self.state.lyrics = Lyrics()
                    self.state.resolving_key = ""
                    self.state.cover = None
                    self.state.hero_cover = None
                    self.state.dirty = True
        elif msg_type == "set-theme":
            await self.set_theme(str(payload.get("theme") or ""))
        await self.render_and_broadcast()

    async def set_track(self, track: TrackInfo) -> None:
        async with self.state_lock:
            if track.key == self.state.track.key and track.title == self.state.track.title:
                return
        cached = await asyncio.to_thread(self.store.manual_or_cached, track)
        async with self.state_lock:
            if track.key == self.state.track.key and track.title == self.state.track.title:
                return
            self._cancel_inflight_resolve()
            self.state.track = track
            self.state.clock.update(0.0, self.state.clock.paused, self.state.clock.playback_rate)
            self.state.lyrics = cached or Lyrics()
            self.state.resolving_key = track.key if cached is None else ""
            # Drop the previous cover immediately; the new one loads async below,
            # and a cache hit is applied before we return so it shows on frame 1.
            cover_url = cover_url_for(track)
            cover_size = self._cover_size()
            hero_size = self._hero_cover_size()
            self.state.cover = (
                self.cover_cache.get((cover_url, cover_size))
                if cover_url and cover_size
                else None
            )
            self.state.hero_cover = (
                self.cover_cache.get((cover_url, hero_size))
                if cover_url and hero_size
                else None
            )
            self.state.dirty = True
        if track.title and cached is None:
            cancel = threading.Event()
            self._resolve_cancel = cancel
            task = asyncio.create_task(self.resolve_track(track, cancel))
            self._background_tasks.add(task)
            task.add_done_callback(self._background_tasks.discard)
        self._ensure_cover(track)

    def _cover_size(self) -> int:
        """Cover decode size for currently connected boards (0 = none want art).

        Keyed off real connections, not the no-board render fallback, so a
        headless daemon does zero cover network I/O.
        """
        return max_cover_size([conn.profile for conn in self.boards])

    def _hero_cover_size(self) -> int:
        """Full-screen art decode size for connected boards (0 = none)."""
        return hero_cover_size([conn.profile for conn in self.boards])

    def _ensure_cover(self, track: TrackInfo) -> None:
        """Fetch this track's cover once, decoded to both the header size and the
        full-screen art size for the largest connected board.

        Only fetches when a board whose profile supports covers is connected, so
        a headless daemon (or a board with no cover slot) does no network I/O.
        Both sizes come from a single HTTP fetch (decoded twice), so hero art
        costs no extra network round trip.
        """
        url = cover_url_for(track)
        small = self._cover_size()
        hero = self._hero_cover_size()
        # TEMP DEBUG: reveals exactly what the extension sent and what we chose.
        print(
            f"[cover] title={track.title!r} video_id={track.video_id!r} "
            f"art_url={track.art_url!r} -> url={url!r} sizes=small:{small},hero:{hero}"
        )
        if not track.title or not url:
            return
        wanted = {s for s in (small, hero) if s > 0}
        needed = sorted(s for s in wanted if (url, s) not in self.cover_cache)
        if not needed:
            return
        task = asyncio.create_task(self.resolve_cover(track, url, needed))
        self._background_tasks.add(task)
        task.add_done_callback(self._background_tasks.discard)

    async def resolve_cover(self, track: TrackInfo, url: str, sizes: list[int]) -> None:
        try:
            decoded = await asyncio.to_thread(self._resolve_cover_sync, url, sizes)
        except Exception as exc:
            print(f"cover fetch failed for {track.title!r}: {exc}")
            # Cache None per size to avoid re-fetching a URL that won't decode.
            decoded = {size: None for size in sizes}
        if len(self.cover_cache) > 64:
            self.cover_cache.clear()
        for size, cover in decoded.items():
            self.cover_cache[(url, size)] = cover
        # Re-read sizes at completion: a board may have (dis)connected mid-fetch.
        small = self._cover_size()
        hero = self._hero_cover_size()
        small_cover = self.cover_cache.get((url, small)) if small > 0 else None
        hero_cover = self.cover_cache.get((url, hero)) if hero > 0 else None
        if small_cover is None and hero_cover is None:
            return
        async with self.state_lock:
            if self.state.track.key != track.key:
                return
            if small_cover is not None:
                self.state.cover = small_cover
            if hero_cover is not None:
                self.state.hero_cover = hero_cover
            self.state.dirty = True
        await self.render_and_broadcast()

    def _resolve_cover_sync(self, url: str, sizes: list[int]) -> dict[int, CoverArt | None]:
        data = fetch_cover_bytes(url)
        decode = getattr(self.renderer, "decode_cover", None)
        if not data or decode is None:  # no bytes, or headless test renderer
            return {size: None for size in sizes}
        return {size: decode(data, size) for size in sizes}

    async def update_clock(self, payload: dict[str, Any], update_duration: bool = True) -> None:
        async with self.state_lock:
            position_sec, duration_sec = self._normalize_playback_timing_locked(
                float(payload.get("positionSec") or 0),
                float(payload.get("durationSec") or 0),
            )
            self.state.clock.update(
                position_sec,
                bool(payload.get("paused")),
                float(payload.get("playbackRate") or 1),
            )
            if update_duration and duration_sec > 0:
                self.state.track.duration_sec = duration_sec
            self.state.dirty = True

    def _normalize_playback_timing_locked(self, position_sec: float, duration_sec: float) -> tuple[float, float]:
        offset = self.media_timeline_offset_sec
        if offset <= 0:
            return position_sec, duration_sec
        if position_sec < max(0.0, offset - 1.0) or (0 < duration_sec <= offset):
            self.media_timeline_offset_sec = 0.0
            return position_sec, duration_sec
        if position_sec >= offset and duration_sec > offset:
            return max(0.0, position_sec - offset), max(0.0, duration_sec - offset)
        return position_sec, duration_sec

    async def resolve_track(self, track: TrackInfo, cancel: threading.Event | None = None) -> None:
        try:
            lyrics = await asyncio.to_thread(self._resolve_track_sync, track, cancel)
        except ResolveCancelled:
            print(f"lyrics resolve cancelled for {track.title!r} (track changed)")
            return
        except Exception as exc:
            if _is_timeout_error(exc):
                print(f"lyrics resolve timed out for {track.title!r}: {exc}")
                lyrics = Lyrics(resolved=True, error="Lyrics fetch timed out")
            else:
                print(f"lyrics resolve failed for {track.title!r}: {exc}")
                lyrics = Lyrics(resolved=True)
        async with self.state_lock:
            if self.state.track.key != track.key:
                return
            self.state.lyrics = lyrics
            self.state.resolving_key = ""
            self.state.dirty = True
        await self.render_and_broadcast()

    def _resolve_track_sync(self, track: TrackInfo, cancel: threading.Event | None = None) -> Lyrics:
        lyrics = fetch_lrclib(track, cancel)
        # Even if the track changed mid-fetch, a completed result is worth
        # caching -- the stale-key check above keeps it off the screen.
        self.store.save_lrclib(track, lyrics)
        return lyrics

    @property
    def last_frame(self) -> bytes | None:
        """Latest default-profile now-frame (kept for tests/back-compat)."""
        return self.last_frames.get(DEFAULT_PROFILE.name)

    @last_frame.setter
    def last_frame(self, frame: bytes | None) -> None:
        if frame is None:
            self.last_frames.pop(DEFAULT_PROFILE.name, None)
        else:
            self.last_frames[DEFAULT_PROFILE.name] = frame

    def _render(self, state: AppState, profile: RenderProfile) -> bytes:
        # During the first five seconds, fill the panel with clean album art.
        # Paused playback uses the same hero art with a PAUSED chip. Both states
        # fall through to lyrics when art is unavailable.
        if state.in_album_art_intro() and state.hero_cover is not None:
            return self._render_hero_art(state, profile, show_paused_chip=False)
        if (
            state.clock.paused
            and state.track.title
            and state.hero_cover is not None
            and hasattr(self.renderer, "render_pause_chip")
        ):
            return self._render_hero_art(state, profile, show_paused_chip=True)
        frame = self.renderer.render(state, profile)
        if self.theme == "light":
            frame = invert_frame(frame)
        # Blit after the theme flip: a photo keeps its natural tonality
        # (luminance -> lit) in both themes, unlike the ink-on-paper UI chrome.
        if state.cover is not None and state.track.title and cover_placement(profile) is not None:
            frame = blit_cover(frame, state.cover, profile)
        return frame

    def _render_hero_art(
        self, state: AppState, profile: RenderProfile, *, show_paused_chip: bool
    ) -> bytes:
        """Full-screen centered album art, optionally with a PAUSED chip.

        Theme-independent -- the photo keeps its natural tonality (the same reason
        covers are blitted post-flip in _render), and the chip is an overlay, not
        ink-on-paper chrome.
        """
        cover = state.hero_cover
        frame = bytes(profile.frame_bytes)  # all-black background (0 = off)
        frame = blit_cover_centered(frame, cover, profile)
        if not show_paused_chip:
            return frame
        value, chip_mask, chip_w, chip_h = self.renderer.render_pause_chip(profile)
        art_x = max(0, (profile.width - cover.size) // 2)
        art_y = max(0, (profile.height - cover.size) // 2)
        margin = 12 if profile.name == SQUARE_PROFILE.name else 14
        frame = composite_chip(
            frame, value, chip_mask, art_x + margin, art_y + margin, chip_w, chip_h, profile
        )
        return frame

    def _active_profiles(self) -> list[RenderProfile]:
        profiles: dict[str, RenderProfile] = {conn.profile.name: conn.profile for conn in self.boards}
        if not profiles:
            profiles[DEFAULT_PROFILE.name] = DEFAULT_PROFILE
        return list(profiles.values())

    def _has_current_track_locked(self) -> bool:
        return bool(self.state.track.title)

    async def set_theme(self, theme: str) -> None:
        theme = theme.strip().lower()
        if theme not in ("dark", "light") or theme == self.theme:
            return
        self.theme = theme

    async def handle_board(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        conn = WebSocketConnection(reader, writer)
        try:
            path = await conn.handshake(self.board_handshake_allowed)
            parsed = urlparse(path)
            query = parse_qs(parsed.query)
            requested_proto = (query.get("proto") or [""])[0]
            legacy = requested_proto == "1" or (
                self.allow_legacy_proto1 and "token" in query and requested_proto != "2"
            )
            if self.identity is not None and not legacy:
                await self.authenticate_board(conn)
            elif self.identity is not None and not self.allow_legacy_proto1:
                raise ValueError("legacy protocol is disabled")
            conn.profile = profile_from_board_path(path)
            conn.supports_rgb565 = board_wants_rgb565(path)
            hello: dict[str, Any] = {
                "type": "hello", "proto": 2 if conn.tx_key else 1,
                "daemonUuid": self.identity.daemon_uuid if self.identity else "",
                "width": conn.profile.width, "height": conn.profile.height,
            }
            if conn.supports_rgb565:
                hello["coverFormat"] = "rgb565be"
            await conn.send_text(hello)
            ready = await asyncio.wait_for(conn.recv_application(), AUTH_TIMEOUT_SECONDS)
            if ready is None or ready[0] != 1:
                raise ValueError("missing protected ready")
            ready_payload = ready[1].decode("utf-8", "replace")
            if conn.tx_key is not None:
                if json.loads(ready_payload).get("type") != "ready":
                    raise ValueError("invalid protected ready")
            elif ready_payload != "ready":
                raise ValueError("invalid legacy ready")
            conn.start_sender()
            self.boards.add(conn)
            color_note = ", color=rgb565" if conn.supports_rgb565 else ""
            print(f"board connected ({conn.profile.name}, proto={'2' if conn.tx_key else '1'}{color_note})")
            # A board may have joined after the track was set; fetch its cover now
            # (no-op if already cached). Also covers the case where the header art
            # is present but this board needs a not-yet-decoded full-screen size.
            async with self.state_lock:
                needs_cover = self.state.cover is None or self.state.hero_cover is None
                pending_cover_track = self.state.track if needs_cover else None
            if pending_cover_track is not None:
                self._ensure_cover(pending_cover_track)
            await self.send_current_frame(conn, now=True)
            heartbeat = asyncio.create_task(conn.heartbeat())
            while True:
                message = await conn.recv_application()
                if message is None:
                    break
                opcode, payload = message
                if opcode == 1 and conn.tx_key is None and payload.decode("utf-8", "replace") == "ready":
                    await self.send_current_frame(conn, now=True)
        except (OSError, ValueError, ConnectionError, asyncio.TimeoutError, json.JSONDecodeError,
                asyncio.IncompleteReadError, asyncio.LimitOverrunError) as exc:
            # IncompleteReadError/LimitOverrunError: a peer opened the socket and
            # closed (or never finished the HTTP request) before the handshake --
            # a probe or a board that dropped. Log it, don't crash the task.
            print(f"board websocket closed: {exc}")
        finally:
            if 'heartbeat' in locals():
                heartbeat.cancel()
                try:
                    await heartbeat
                except (asyncio.CancelledError, ConnectionError):
                    pass
            self.boards.discard(conn)
            conn.close()
            print("board disconnected")

    async def authenticate_board(self, conn: WebSocketConnection) -> str:
        assert self.identity is not None
        server_nonce = secrets.token_bytes(32)
        await conn.send_text({"type": "auth-challenge", "proto": 2,
                              "daemonUuid": self.identity.daemon_uuid,
                              "serverNonce": server_nonce.hex()})
        message = await asyncio.wait_for(conn.recv(), AUTH_TIMEOUT_SECONDS)
        if message is None or message[0] != 1:
            raise ValueError("missing authentication response")
        response = json.loads(message[1].decode("utf-8"))
        if response.get("type") != "auth-response" or response.get("proto") != 2:
            raise ValueError("invalid authentication response")
        client_nonce = bytes.fromhex(str(response.get("clientNonce", "")))
        proof = bytes.fromhex(str(response.get("proof", "")))
        board_id = str(response.get("boardId", ""))
        if len(client_nonce) != 32 or not re.fullmatch(r"[0-9A-Fa-f:-]{12,32}", board_id):
            raise ValueError("invalid board identity or nonce")
        expected = hmac.new(self.identity.token.encode("ascii"),
                            _auth_transcript("client", self.identity.daemon_uuid, server_nonce, client_nonce),
                            hashlib.sha256).digest()
        if not hmac.compare_digest(proof, expected):
            raise ValueError("board authentication failed")
        server_proof = hmac.new(self.identity.token.encode("ascii"),
                                _auth_transcript("server", self.identity.daemon_uuid, server_nonce, client_nonce),
                                hashlib.sha256).hexdigest()
        await conn.send_text({"type": "auth-ok", "proto": 2, "proof": server_proof})
        client_to_server, server_to_client = derive_session_keys(self.identity.token, server_nonce, client_nonce)
        conn.enable_security(server_to_client, client_to_server)
        return board_id

    def board_handshake_allowed(self, path: str, headers: dict[str, str]) -> bool:
        parsed = urlparse(path)
        if parsed.path != "/board":
            return False
        query = parse_qs(parsed.query)
        requested_proto = (query.get("proto") or [""])[0]
        legacy = requested_proto == "1" or (
            self.allow_legacy_proto1 and "token" in query and requested_proto != "2"
        )
        if self.identity is not None and not legacy:
            return True
        if self.identity is not None and not self.allow_legacy_proto1:
            return False
        if not self.board_token:
            return self.identity is None
        query_token = (query.get("token") or [""])[0]
        auth = headers.get("authorization", "")
        bearer = auth[7:].strip() if auth.lower().startswith("bearer ") else ""
        return hmac.compare_digest(query_token, self.board_token) or hmac.compare_digest(bearer, self.board_token)

    async def render_and_broadcast(self) -> None:
        async with self.state_lock:
            self.schedule_generation += 1
            generation = self.schedule_generation
            if self.scheduled_task is not None:
                self.scheduled_task.cancel()
                self.scheduled_task = None
            if not self._has_current_track_locked():
                self.state.dirty = False
                plan = None
                frames = None
                display_state = None
            else:
                profiles = self._active_profiles()
                display_state = self.state.at_position(self.state.clock.interpolated_position())
                frames = {profile.name: self._render(display_state, profile) for profile in profiles}
                self.state.dirty = False
                plan = self._next_scheduled_frame_locked(profiles)
        if frames is None:
            self.last_frames.clear()
            await self.broadcast_clear()
            return
        self.last_frames.update(frames)
        await self.broadcast_frames(frames, now=True, swap_in_ms=0, display_state=display_state)
        await self.arm_scheduled_frame(plan, generation)

    async def broadcast_clear(self) -> None:
        dead: list[WebSocketConnection] = []
        for board in list(self.boards):
            try:
                await self.send_clear(board)
            except OSError:
                dead.append(board)
        for board in dead:
            self.boards.discard(board)
            board.close()

    async def send_clear(self, conn: WebSocketConnection) -> None:
        await conn.send_text({"type": "clear"})
        conn.board_frame_base = None
        conn.board_color_key = None

    async def send_current_frame(self, conn: WebSocketConnection, now: bool) -> None:
        profile = conn.profile
        async with self.state_lock:
            has_track = self._has_current_track_locked()
            if not has_track:
                self.last_frames.clear()
                frame = None
                plan = None
            else:
                display_state = self.state.at_position(self.state.clock.interpolated_position())
                frame = self.last_frames.get(profile.name)
                if frame is None:
                    frame = self._render(display_state, profile)
                    self.last_frames[profile.name] = frame
                plan = self._next_scheduled_frame_locked([profile])
        if not has_track or frame is None:
            await self.send_clear(conn)
            return
        painted = await self.send_frame(conn, frame, now=now, swap_in_ms=0)
        if now:
            await self.send_color_cover(conn, display_state, painted)
        if plan is not None:
            await self.send_frame(conn, plan.frames[profile.name], now=False, swap_in_ms=plan.swap_in_ms)

    async def send_frame(
        self, conn: WebSocketConnection, frame: bytes, now: bool, swap_in_ms: int
    ) -> tuple[int, int, int, int] | None:
        profile = conn.profile
        # The board already shows this exact frame -- skip the resend. Without
        # this, a paused re-render (theme toggle, a late cover resolve) would push
        # an identical full-screen photo and flash the panel. Only for immediate
        # frames: scheduled frames are now=False, and a fresh/cleared board has
        # board_frame_base=None so it still gets the frame.
        if now and swap_in_ms <= 0 and conn.board_frame_base == frame:
            return None
        rect = (
            dirty_rect(conn.board_frame_base, frame, profile.width, profile.height)
            if now and swap_in_ms <= 0
            else None
        )
        if rect is not None and rect.bytes < len(frame):
            envelope = make_frame_envelope(
                FRAME_KIND_RECT_NOW,
                rect.payload,
                x=rect.x,
                y=rect.y,
                rect_width=rect.width,
                rect_height=rect.height,
                row_bytes=rect.row_bytes,
                swap_in_ms=0,
                display_width=profile.width,
                display_height=profile.height,
            )
            await conn.send_binary(envelope)
            conn.board_frame_base = frame
            return (rect.x, rect.y, rect.width, rect.height)

        kind = FRAME_KIND_FULL_NOW if now else FRAME_KIND_FULL_SCHEDULED
        envelope = make_frame_envelope(
            kind,
            frame,
            x=0,
            y=0,
            rect_width=profile.width,
            rect_height=profile.height,
            row_bytes=profile.row_bytes,
            swap_in_ms=max(0, int(swap_in_ms)),
            display_width=profile.width,
            display_height=profile.height,
        )
        await conn.send_binary(envelope)
        if now and swap_in_ms <= 0:
            conn.board_frame_base = frame
            return (0, 0, profile.width, profile.height)
        return None

    def _color_cover_for_state(self, state: AppState, profile: RenderProfile) -> ColorRect | None:
        hero = (
            state.in_album_art_intro()
            or (
                state.clock.paused
                and state.track.title
                and state.hero_cover is not None
                and hasattr(self.renderer, "render_pause_chip")
            )
        )
        if hero and state.hero_cover is not None:
            rect = color_cover_rect(
                state.hero_cover,
                profile,
                slot_x=0,
                slot_y=0,
                slot_width=profile.width,
                slot_height=profile.height,
            )
            if rect is None or not state.clock.paused or not hasattr(self.renderer, "render_pause_chip"):
                return rect
            value, mask, chip_w, chip_h = self.renderer.render_pause_chip(profile)  # type: ignore[attr-defined]
            cover = state.hero_cover
            art_x = max(0, (profile.width - cover.size) // 2)
            art_y = max(0, (profile.height - cover.size) // 2)
            margin = 12 if profile.name == SQUARE_PROFILE.name else 14
            return composite_rgb565_chip(
                rect, value, mask, art_x + margin, art_y + margin, chip_w, chip_h
            )

        place = cover_placement(profile)
        if state.cover is None or not state.track.title or place is None:
            return None
        return color_cover_rect(
            state.cover,
            profile,
            slot_x=place.x,
            slot_y=place.y,
            slot_width=place.size,
            slot_height=place.size,
        )

    async def send_color_cover(
        self,
        conn: WebSocketConnection,
        state: AppState,
        painted: tuple[int, int, int, int] | None,
    ) -> None:
        """Restore true-color art after a mono frame/rect touches its pixels."""
        if not conn.supports_rgb565:
            return
        rect = self._color_cover_for_state(state, conn.profile)
        if rect is None:
            conn.board_color_key = None
            return
        digest = hashlib.sha256(rect.payload).digest()
        key = (rect.x, rect.y, rect.width, rect.height, digest)
        touched = False
        if painted is not None:
            px, py, pw, ph = painted
            touched = not (
                px + pw <= rect.x or rect.x + rect.width <= px
                or py + ph <= rect.y or rect.y + rect.height <= py
            )
        if not touched and conn.board_color_key == key:
            return
        await conn.send_binary(make_color_envelope(rect, conn.profile))
        conn.board_color_key = key

    def _next_scheduled_frame_locked(self, profiles: list[RenderProfile]) -> ScheduledFrame | None:
        # While hero art hides the lyrics, the next visible change is the end of
        # the five-second intro. Schedule that exact lyrics frame instead of an
        # earlier hidden line transition.
        if self.state.in_album_art_intro() and self.state.hero_cover is not None:
            next_ms = int(ALBUM_ART_INTRO_SECONDS * 1000)
        else:
            next_ms = self.state.next_line_time_ms()
        if next_ms is None:
            return None
        position_ms = int(self.state.clock.interpolated_position() * 1000)
        remaining_ms = int((next_ms - position_ms) / max(self.state.clock.playback_rate, 0.001))
        if remaining_ms <= MIN_SCHEDULE_SWAP_MS:
            return None
        future_state = self.state.at_position(next_ms / 1000.0)
        frames = {profile.name: self._render(future_state, profile) for profile in profiles}
        return ScheduledFrame(
            frames=frames,
            swap_in_ms=remaining_ms,
            due_monotonic_ms=monotonic_ms() + remaining_ms,
        )

    async def broadcast_frames(
        self,
        frames: dict[str, bytes],
        now: bool,
        swap_in_ms: int,
        display_state: AppState | None = None,
    ) -> None:
        dead: list[WebSocketConnection] = []
        # Snapshot: send_frame awaits, during which a board may connect/disconnect
        # and mutate self.boards, which would raise "Set changed size during iteration".
        for board in list(self.boards):
            frame = frames.get(board.profile.name)
            if frame is None:
                # Board connected between render and broadcast; its connect-time
                # send_current_frame sync covers it.
                continue
            try:
                painted = await self.send_frame(board, frame, now=now, swap_in_ms=swap_in_ms)
                if now and display_state is not None:
                    await self.send_color_cover(board, display_state, painted)
            except OSError:
                dead.append(board)
        for board in dead:
            self.boards.discard(board)
            board.close()

    async def arm_scheduled_frame(self, plan: ScheduledFrame | None, generation: int) -> None:
        if plan is None:
            return
        async with self.state_lock:
            if generation != self.schedule_generation:
                return
        await self.broadcast_frames(plan.frames, now=False, swap_in_ms=plan.swap_in_ms)
        async with self.state_lock:
            if generation != self.schedule_generation:
                return
            self.scheduled_task = asyncio.create_task(
                self.advance_schedule_after(plan.due_monotonic_ms, generation)
            )

    async def advance_schedule_after(self, due_monotonic_ms: int, generation: int) -> None:
        try:
            await asyncio.sleep(max(0, due_monotonic_ms - monotonic_ms()) / 1000.0)
            async with self.state_lock:
                if generation != self.schedule_generation:
                    return
                # A scheduled swap just became due, so the board's one-shot timer
                # should have flipped it to the scheduled frame -- but we cannot
                # rely on which frame it actually holds (the firmware can skip a
                # pending frame when a newer scheduled frame overwrites it, or when
                # a now-frame cancels it). Instead of guessing the base, invalidate
                # it so the next now-frame is sent full and resyncs the board. This
                # prevents a tight rect against a wrong base from leaving stale
                # pixels (e.g. long -> short line ghosting, or a clipped highlight).
                for board in list(self.boards):
                    board.board_frame_base = None
                profiles = self._active_profiles()
                self.last_frames = {profile.name: self._render(self.state, profile) for profile in profiles}
                plan = self._next_scheduled_frame_locked(profiles)
            await self.arm_scheduled_frame(plan, generation)
        except asyncio.CancelledError:
            return


def mdns_encode_name(name: str) -> bytes:
    out = bytearray()
    for label in name.rstrip(".").split("."):
        raw = label.encode("utf-8")
        if len(raw) > 63:
            raw = raw[:63]
        out.append(len(raw))
        out.extend(raw)
    out.append(0)
    return bytes(out)


def mdns_read_name(packet: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    jumped = False
    next_offset = offset
    seen = 0
    while offset < len(packet) and seen < 32:
        seen += 1
        length = packet[offset]
        if length == 0:
            offset += 1
            if not jumped:
                next_offset = offset
            break
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                break
            pointer = ((length & 0x3F) << 8) | packet[offset + 1]
            if not jumped:
                next_offset = offset + 2
            offset = pointer
            jumped = True
            continue
        offset += 1
        labels.append(packet[offset : offset + length].decode("utf-8", "replace"))
        offset += length
        if not jumped:
            next_offset = offset
    return ".".join(labels).lower(), next_offset


def mdns_query_names(packet: bytes) -> list[str]:
    if len(packet) < 12:
        return []
    qdcount = struct.unpack("!H", packet[4:6])[0]
    offset = 12
    names: list[str] = []
    for _idx in range(qdcount):
        name, offset = mdns_read_name(packet, offset)
        if offset + 4 > len(packet):
            break
        offset += 4
        names.append(name.rstrip("."))
    return names


def mdns_rr(name: str, rrtype: int, rrclass: int, ttl: int, rdata: bytes) -> bytes:
    return mdns_encode_name(name) + struct.pack("!HHIH", rrtype, rrclass, ttl, len(rdata)) + rdata


def local_ipv4() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return str(sock.getsockname()[0])
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"


class DnsSdAdvertiser:
    def __init__(self, instance: str, service: str, proto: str, port: int, txt: dict[str, str] | None = None) -> None:
        self.instance = instance
        self.service = service
        self.proto = proto
        self.port = port
        self.txt = txt or {}
        self.proc: subprocess.Popen[bytes] | None = None
        self.sock: socket.socket | None = None
        self.stop_event = threading.Event()
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        # On macOS, Bonjour owns interface scoping and publishes the correct A
        # records per interface (including when VPNs come and go). Prefer it to
        # synthesizing one host address ourselves.
        if shutil.which("dns-sd") is not None:
            self._start_dns_sd()
            return
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            self.sock.bind(("", MDNS_PORT))
            mreq = socket.inet_aton(MDNS_GROUP) + socket.inet_aton("0.0.0.0")
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            self.sock.settimeout(0.5)
        except OSError as exc:
            print(f"native mDNS unavailable ({exc}); trying dns-sd fallback")
            self.sock = None
            self._start_dns_sd()
            return
        self.thread = threading.Thread(target=self._serve, name="lyrics-mdns", daemon=True)
        self.thread.start()
        self._send_announcement()
        print(f"mDNS advertisement: {self.instance}.{self.service}.{self.proto}.local:{self.port}")

    def _reap_stale_dns_sd(self) -> None:
        # A daemon that exits without unwinding never reaches stop(), so its
        # dns-sd -R child survives reparented to init and keeps advertising a
        # stale TXT record. Because every run registers the same instance name,
        # Bonjour lets the oldest holder own it -- boards then read the dead
        # daemon's TXT (e.g. proto=1 from an old --insecure run), reject it, and
        # sit in RETRY_WAIT forever. Nothing we do at shutdown can cover a
        # SIGKILL, so clear the name at startup instead.
        needle = f"-R {self.instance} {self.service}.{self.proto} "
        try:
            listing = subprocess.run(["ps", "-axo", "pid=,command="],
                                     capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            return
        for line in listing.splitlines():
            pid_text, _, command = line.strip().partition(" ")
            if "dns-sd" not in command or needle not in command:
                continue
            try:
                os.kill(int(pid_text), signal.SIGTERM)
            except (ValueError, OSError):
                continue
            print(f"reaped stale dns-sd registration (pid {pid_text})")

    def _start_dns_sd(self) -> None:
        dns_sd = shutil.which("dns-sd")
        if dns_sd is None:
            print("dns-sd unavailable; _lyrics._tcp mDNS advertisement disabled")
            return
        self._reap_stale_dns_sd()
        try:
            self.proc = subprocess.Popen(
                [dns_sd, "-R", self.instance, f"{self.service}.{self.proto}", "local", str(self.port),
                 *[f"{key}={value}" for key, value in self.txt.items()]],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError as exc:
            print(f"dns-sd failed to start ({exc}); _lyrics._tcp mDNS advertisement disabled")
            return
        print(f"mDNS advertisement: {self.instance}.{self.service}.{self.proto}.local:{self.port}")

    @property
    def service_name(self) -> str:
        return f"{self.service}.{self.proto}.local"

    @property
    def instance_name(self) -> str:
        return f"{self.instance}.{self.service}.{self.proto}.local"

    @property
    def host_name(self) -> str:
        safe = re.sub(r"[^a-zA-Z0-9-]+", "-", socket.gethostname()).strip("-") or "g4pys-lyrics"
        return f"{safe}.local"

    def _response_packet(self) -> bytes:
        txt_items = [f"{key}={value}".encode("utf-8")[:255] for key, value in self.txt.items()]
        txt_rdata = b"".join(bytes([len(item)]) + item for item in txt_items) or b"\x00"
        answers = [
            mdns_rr(self.service_name, 12, 1, 120, mdns_encode_name(self.instance_name)),
            mdns_rr(self.instance_name, 33, 0x8001, 120, struct.pack("!HHH", 0, 0, self.port) + mdns_encode_name(self.host_name)),
            mdns_rr(self.instance_name, 16, 0x8001, 120, txt_rdata),
            mdns_rr(self.host_name, 1, 0x8001, 120, socket.inet_aton(local_ipv4())),
        ]
        return struct.pack("!HHHHHH", 0, 0x8400, 0, len(answers), 0, 0) + b"".join(answers)

    def _send_announcement(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.sendto(self._response_packet(), (MDNS_GROUP, MDNS_PORT))
        except OSError:
            pass

    def _serve(self) -> None:
        assert self.sock is not None
        last_announce = 0.0
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - last_announce > 30:
                self._send_announcement()
                last_announce = now
            try:
                packet, _addr = self.sock.recvfrom(2048)
            except TimeoutError:
                continue
            except OSError:
                break
            names = mdns_query_names(packet)
            if any(name in {self.service_name, self.instance_name, self.host_name} for name in names):
                self._send_announcement()

    def stop(self) -> None:
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.thread is not None:
            self.thread.join(timeout=1)
            self.thread = None
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=2)
        self.proc = None


def build_renderer(font_name: str) -> FrameRenderer:
    try:
        return CoreTextFrameRenderer(font_name)
    except ImportError as exc:
        print(
            f"Core Text renderer unavailable ({exc}); using geometry-only fallback "
            "(rules/progress only, no text)"
        )
        return FallbackFrameRenderer()


def default_db_path() -> str:
    return str(Path.home() / ".g4pys" / "lyrics-display.sqlite3")


def track_from_import_args(args: argparse.Namespace, file_path: Path) -> TrackInfo:
    title = args.title or file_path.stem
    return TrackInfo(
        video_id=args.video_id or file_path.stem,
        title=title,
        artist=args.artist or "",
        album=args.album or "",
        duration_sec=float(args.duration or 0),
    )


def import_lrc_command(args: argparse.Namespace) -> int:
    store = LyricsStore(Path(args.db))
    imported = 0
    for filename in args.files:
        file_path = Path(filename)
        text = file_path.read_text(encoding="utf-8")
        synced, syllables = parse_lrc_with_syllables(text)
        if synced:
            lyrics = Lyrics(synced=synced, syllables=syllables, resolved=True)
        else:
            lyrics = Lyrics(plain=[line.strip() for line in text.splitlines() if line.strip()], resolved=True)
        store.save_manual(track_from_import_args(args, file_path), lyrics)
        imported += 1
    print(f"imported {imported} manual LRC file(s)")
    return 0


def cache_command(args: argparse.Namespace) -> int:
    store = LyricsStore(Path(args.db))
    if args.cache_command == "clear":
        deleted = store.clear_cache(source=args.source, video_id=args.video_id)
        print(f"deleted {deleted} row(s) where source={args.source!r}")
        return 0
    if args.cache_command == "stats":
        rows = store.stats()
        if not rows:
            print("cache is empty")
            return 0
        for row in rows:
            print(f"{row['source']}: rows={row['rows']} negative={row['negative_rows'] or 0}")
        return 0
    raise ValueError(f"unknown cache command {args.cache_command}")


async def run(args: argparse.Namespace) -> None:
    store = LyricsStore(Path(args.db))
    # --insecure: run with no identity at all. Boards then connect over proto=1
    # with no token and frames go out unwrapped (no SEC2/HMAC). This is what the
    # ESP8266 needs -- it lacks the heap to buffer-and-verify a whole SEC2 record
    # on top of its framebuffer. Trades LAN-link authentication for ~7 KB of heap.
    if args.insecure:
        identity = None
    else:
        identity = load_or_create_identity(Path(args.identity))
        if args.board_token:
            if not re.fullmatch(r"[0-9a-fA-F]{64}", args.board_token):
                raise ValueError("--board-token must be exactly 64 hexadecimal characters")
            identity = DaemonIdentity(identity.daemon_uuid, args.board_token)
    daemon = LyricsDisplayDaemon(store, build_renderer(args.font),
                                 board_token=identity.token if identity else "",
                                 identity=identity, allow_legacy_proto1=args.allow_legacy_proto1)
    daemon_uuid = identity.daemon_uuid if identity else ""
    advertiser = DnsSdAdvertiser(
        args.mdns_instance,
        "_lyrics",
        "_tcp",
        args.board_port,
        txt=({"path": "/board", "proto": "2", "auth": "hmac-sha256", "uuid": daemon_uuid}
             if identity else
             {"path": "/board?proto=1", "proto": "1", "auth": "none"}),
    )
    legacy_advertiser = DnsSdAdvertiser(
        args.mdns_instance + " Legacy", "_lyrics", "_tcp", args.board_port,
        txt={"path": "/board?proto=1", "proto": "1", "auth": "token", "uuid": daemon_uuid},
    ) if (identity and args.allow_legacy_proto1) else None
    extension_server = await asyncio.start_server(
        daemon.handle_extension, args.extension_host, args.extension_port
    )
    board_server = await asyncio.start_server(daemon.handle_board, args.board_host, args.board_port)
    print(f"extension WebSocket: ws://{args.extension_host}:{args.extension_port}/extension")
    print(f"board WebSocket: ws://{args.board_host}:{args.board_port}/board")
    if identity:
        print(f"secure board identity: {identity.daemon_uuid}")
        print("board WebSocket auth: mutual nonce/HMAC (proto=2)")
    else:
        print("board WebSocket auth: DISABLED (--insecure): proto=1, unauthenticated")
    if not args.no_mdns:
        advertiser.start()
        if legacy_advertiser is not None:
            legacy_advertiser.start()
    # SIGTERM and SIGHUP (closed terminal) otherwise kill us outright, skipping
    # the finally below and orphaning the dns-sd child. Turn them into a normal
    # unwind so stop() runs. SIGKILL still can't be caught -- _reap_stale_dns_sd
    # covers that case on the next start.
    loop = asyncio.get_running_loop()
    stopping = loop.create_future()
    for sig in (signal.SIGTERM, signal.SIGHUP):
        try:
            loop.add_signal_handler(
                sig, lambda: stopping.done() or stopping.set_result(None))
        except (NotImplementedError, RuntimeError):
            pass
    try:
        async with extension_server, board_server:
            serving = [asyncio.ensure_future(extension_server.serve_forever()),
                       asyncio.ensure_future(board_server.serve_forever())]
            try:
                await asyncio.wait(serving + [stopping], return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in serving:
                    task.cancel()
                await asyncio.gather(*serving, return_exceptions=True)
    finally:
        advertiser.stop()
        if legacy_advertiser is not None:
            legacy_advertiser.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command")

    serve_parser = subparsers.add_parser("serve", help="run the daemon")
    for target in (parser, serve_parser):
        target.add_argument("--extension-host", default=os.environ.get("G4PYS_LYRICS_EXTENSION_HOST", EXTENSION_HOST))
        target.add_argument("--extension-port", type=int, default=int(os.environ.get("G4PYS_LYRICS_EXTENSION_PORT", str(EXTENSION_PORT))))
        target.add_argument("--board-host", default=os.environ.get("G4PYS_LYRICS_BOARD_HOST", BOARD_HOST))
        target.add_argument("--board-port", type=int, default=int(os.environ.get("G4PYS_LYRICS_BOARD_PORT", str(BOARD_PORT))))
        target.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
        target.add_argument("--font", default=os.environ.get("G4PYS_LYRICS_FONT", "Sukhumvit Set Semi Bold"))
        target.add_argument("--mdns-instance", default=os.environ.get("G4PYS_LYRICS_MDNS_INSTANCE", "g4pys Lyrics Display"))
        target.add_argument("--board-token", default=os.environ.get("G4PYS_LYRICS_BOARD_TOKEN", ""))
        target.add_argument("--identity", default=os.environ.get("G4PYS_LYRICS_IDENTITY", str(default_identity_path())))
        target.add_argument("--allow-legacy-proto1", action="store_true",
                            default=os.environ.get("G4PYS_LYRICS_ALLOW_LEGACY_PROTO1") == "1")
        target.add_argument("--insecure", action="store_true",
                            default=os.environ.get("G4PYS_LYRICS_INSECURE") == "1",
                            help="run with no board identity: boards connect unauthenticated "
                                 "(proto=1, unwrapped frames, no pairing token). Needed for the "
                                 "ESP8266, which lacks the heap for the SEC2 record layer.")
        target.add_argument("--no-mdns", action="store_true", default=os.environ.get("G4PYS_LYRICS_NO_MDNS") == "1")

    import_parser = subparsers.add_parser("import-lrc", help="import one or more .lrc files as manual lyrics")
    import_parser.add_argument("files", nargs="+")
    import_parser.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
    import_parser.add_argument("--video-id", default="")
    import_parser.add_argument("--title", default="")
    import_parser.add_argument("--artist", default="")
    import_parser.add_argument("--album", default="")
    import_parser.add_argument("--duration", type=float, default=0.0)

    cache_parser = subparsers.add_parser("cache", help="inspect or clear cached lyrics")
    cache_parser.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
    cache_subparsers = cache_parser.add_subparsers(dest="cache_command", required=True)
    clear_parser = cache_subparsers.add_parser("clear", help="delete cache rows by source")
    clear_parser.add_argument("--source", default="lrclib", choices=("lrclib", "manual"))
    clear_parser.add_argument("--video-id", default="")
    cache_subparsers.add_parser("stats", help="show cache row counts")

    pairing_parser = subparsers.add_parser("pairing-token", help="print the token to enter on a board")
    pairing_parser.add_argument("--identity", default=os.environ.get("G4PYS_LYRICS_IDENTITY", str(default_identity_path())))

    args = parser.parse_args()
    if args.command == "import-lrc":
        return import_lrc_command(args)
    if args.command == "cache":
        return cache_command(args)
    if args.command == "pairing-token":
        identity = load_or_create_identity(Path(args.identity))
        print(f"daemon UUID: {identity.daemon_uuid}")
        print(f"pairing token: {identity.token}")
        return 0
    if args.command == "serve":
        asyncio.run(run(args))
        return 0
    # Backward-compatible default: options without a subcommand still run the daemon.
    asyncio.run(run(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

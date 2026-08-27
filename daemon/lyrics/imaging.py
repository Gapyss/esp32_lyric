"""Pixel work: cover dithering/blitting, lyric layout fitting, dirty rects and
the LYR1/ART1 wire envelopes."""

from __future__ import annotations

import math
import urllib.parse
import urllib.request
from urllib.parse import parse_qs, urlparse

from .constants import COLOR_ENVELOPE_MAGIC, COLOR_ENVELOPE_VERSION, COLOR_KIND_RECT_NOW, DISPLAY_HEIGHT, DISPLAY_WIDTH, FRAME_ENVELOPE_MAGIC, FRAME_ENVELOPE_STRUCT, FRAME_ENVELOPE_VERSION, LRCLIB_USER_AGENT, LyricBreakFn
from .state import ColorRect, CoverArt, DirtyRect
from .profiles import DEFAULT_PROFILE, LyricLayout, LyricLayoutSizes, PROFILES_BY_SIZE, RenderProfile, cover_placement

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

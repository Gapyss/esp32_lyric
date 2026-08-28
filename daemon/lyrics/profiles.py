"""Board render profiles -- one per display geometry the daemon serves."""

from __future__ import annotations

from dataclasses import dataclass

from .constants import DISPLAY_HEIGHT, DISPLAY_WIDTH, LYRIC_BAND_BOTTOM_Y, LYRIC_BAND_TOP_Y, LYRIC_BASE_SIZE, LYRIC_BOX_WIDTH, LYRIC_BOX_X, LYRIC_HIGHLIGHT_GAP_Y, LYRIC_MIN_SIZE, LYRIC_SINGLE_BASELINE_Y, LYRIC_SINGLE_HIGHLIGHT_Y, LYRIC_WRAP_SIZE

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

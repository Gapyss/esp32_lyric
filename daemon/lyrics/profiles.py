"""Board render profiles -- one per display geometry the daemon serves.

This module is deliberately board-agnostic: it defines the *shape* of a profile
and the registry a process serves, but names no board. Each board family lives
in its own ``board_*`` module and registers itself, which is what lets the
ESP8266 run as a process that never imports the ESP32 layout (and vice versa).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable

from .constants import (
    LYRIC_BAND_TOP_Y,
    LYRIC_BASE_SIZE,
    LYRIC_HIGHLIGHT_GAP_Y,
    LYRIC_MIN_SIZE,
    LYRIC_SINGLE_BASELINE_Y,
    LYRIC_SINGLE_HIGHLIGHT_Y,
    LYRIC_WRAP_SIZE,
)


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


# Album-art cover: a dithered square in the header's top-right, above the
# progress row, wrapped in a Tend hairline "card" frame. Title/artist reflow
# into the left column when a cover is present; the karaoke lyric band below is
# untouched. One placement per board profile.
@dataclass(frozen=True)
class CoverPlacement:
    x: int          # art top-left (top-origin), the reserved blit rect
    y: int
    size: int       # square art edge, in pixels
    title_w: int    # header left-column width for title/artist when art present
    frame_pad: int  # gap between art edge and the hairline card frame


@dataclass(frozen=True)
class PauseChipGeom:
    """Type metrics for the PAUSED pill composited over full-screen album art."""

    size: int
    pad_x: int
    chip_h: int
    rise: int
    # Inset of the pill from the art's top-left corner when it is composited.
    margin: int


@dataclass(frozen=True)
class RenderProfile:
    """One board display geometry the daemon can render frames for.

    Boards opt into a profile with /board?w=&h= at handshake time. Everything
    that used to be selected by comparing ``profile.name`` against a board
    constant now hangs off the profile itself -- the progress row geometry, the
    cover slot, the pause chip metrics, and ``paint``, the function that draws a
    full frame for this geometry. ``paint`` is called as
    ``profile.paint(renderer, ctx, state, profile)``.
    """

    name: str
    width: int
    height: int
    lyric_sizes: LyricLayoutSizes
    lyric_box_x: int
    lyric_box_width: int
    lyric_band_bottom_y: int
    progress_geom: ProgressGeom
    pause_chip: PauseChipGeom
    paint: Callable[[Any, Any, Any, "RenderProfile"], None]
    cover: CoverPlacement | None = None

    @property
    def row_bytes(self) -> int:
        return self.width // 8

    @property
    def frame_bytes(self) -> int:
        return self.width * self.height // 8


class BoardRegistry:
    """The set of board geometries one daemon process serves.

    Split out per process: the ESP32 daemon registers only the 400x300 profile
    and the ESP8266 daemon only the 240x240 one, so an unparseable /board query
    falls back to a geometry that actually fits the screen on the other end. A
    single shared registry would fall back to the ESP32's 400x300 and paint an
    oversized frame at a 240x240 panel.
    """

    def __init__(self) -> None:
        self._by_size: dict[tuple[int, int], RenderProfile] = {}
        self._default: RenderProfile | None = None

    def register(self, profile: RenderProfile, *, default: bool = False) -> RenderProfile:
        self._by_size[(profile.width, profile.height)] = profile
        if default or self._default is None:
            self._default = profile
        return profile

    def reset(self) -> None:
        self._by_size.clear()
        self._default = None

    @property
    def default(self) -> RenderProfile:
        if self._default is None:
            raise RuntimeError(
                "no board profile registered: import a daemon.lyrics.board_* module "
                "(or call BOARDS.register) before rendering"
            )
        return self._default

    def profile_for(self, width: int, height: int) -> RenderProfile:
        return self._by_size.get((width, height), self.default)

    def all(self) -> list[RenderProfile]:
        return list(self._by_size.values())


# Process-wide registry. Board modules append to it at import time; the CLI
# decides which board modules a given process imports.
BOARDS = BoardRegistry()


def cover_placement(profile: "RenderProfile") -> CoverPlacement | None:
    return profile.cover


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

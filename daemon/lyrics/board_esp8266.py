"""The 240x240 GeekMagic SmallTV board (ESP8266) -- geometry and painter.

The split line is really "compact 240x240 colour board", not the chip: a board
opts in with ``/board?w=240&h=240`` and negotiates the RGB565 cover overlay with
``color=rgb565``. The ESP8266 is simply the only thing that does so today, and
it is the reason this lives apart -- it speaks unauthenticated proto=1 (no heap
for the SEC2 record layer) and dials a hardcoded port, so it gets its own
process rather than dragging the ESP32 down to its auth posture.

Importing this module registers the 240x240 profile with :data:`BOARDS`.
"""

from __future__ import annotations

from typing import Any

from .constants import MONO_FONT_NAME, TRACKING_MEGA, TRACKING_WIDE
from .profiles import (
    BOARDS,
    CoverPlacement,
    LyricLayoutSizes,
    PauseChipGeom,
    ProgressGeom,
    RenderProfile,
    cover_placement,
)
from .state import AppState

# Compact 240x240 lyric band. Same fitting algorithm as the 400x300 band, just
# smaller sizes and a tighter box.
SQUARE_LYRIC_LAYOUT_SIZES = LyricLayoutSizes(
    base_size=22,
    wrap_size=18,
    min_size=12,
    band_top_y=104,
    single_baseline_y=146,
    single_highlight_y=162,
    highlight_gap_y=16,
)
SQUARE_PROGRESS_GEOM = ProgressGeom(
    time_size=11, left_x=12, left_w=48, right_x=180, right_w=48,
    baseline_y=95, bar_x=66, bar_w=108, center_y=91, dot_r=3.5,
)
SQUARE_COVER_PLACEMENT = CoverPlacement(x=174, y=30, size=52, title_w=174 - 12 - 8, frame_pad=2)
SQUARE_PAUSE_CHIP = PauseChipGeom(size=11, pad_x=7, chip_h=16, rise=11, margin=12)


def draw_square(renderer: Any, ctx: Any, state: AppState, profile: RenderProfile) -> None:
    """Compact 240x240 layout for the ESP8266 SmallTV boards.

    Same structure as the wide layout -- eyebrow, title/artist, progress,
    karaoke lyric band, next line, footer -- just tighter type scale.
    """
    width = profile.width
    label_font = renderer._role_font("label")
    display_font = renderer._role_font("display")
    body_font = renderer._role_font("body")
    body_medium_font = renderer._role_font("body_medium")

    renderer._draw_text(
        ctx, "NOW PLAYING", 11, 12, 14, width - 24, "left", font_name=label_font, tracking=TRACKING_MEGA
    )
    renderer._draw_rule(ctx, 10, 24, width - 20)

    if not state.track.title:
        renderer._draw_text(ctx, "nothing playing", 22, 12, 112, width - 24, "center", font_name=display_font)
        renderer._draw_text(
            ctx, "waiting for youtube music", 13, 12, 140, width - 24, "center", font_name=body_font
        )
        renderer._screen_rect(ctx, 12, 126, width - 24, 18)
        renderer._draw_text(
            ctx, "1.0.0", 11, 116, 228, width - 128, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
        )
        return

    place = cover_placement(profile) if state.cover is not None else None
    header_w = place.title_w if place is not None else width - 24
    renderer._draw_text(ctx, state.track.title, 20, 12, 52, header_w, "left", font_name=display_font)
    renderer._draw_text(ctx, state.track.artist, 14, 12, 74, header_w, "left", font_name=body_medium_font)
    if place is not None:
        renderer._draw_cover_card(ctx, place)
    renderer._draw_progress(ctx, state, MONO_FONT_NAME, profile.progress_geom)
    renderer._draw_rule(ctx, 10, 104, width - 20)

    _current, next_line, third = state.current_lines()
    current_layout = renderer._draw_lyric_band(ctx, state, profile)
    renderer._draw_text(ctx, next_line, 14, 12, 202, width - 24, "center", font_name=body_font)
    if not current_layout.hide_third and third:
        renderer._draw_text(ctx, third, 12, 12, 222, width - 24, "center", font_name=body_font)
        renderer._screen_rect(ctx, 12, 210, width - 24, 16)
    if state.clock.paused:
        chip = profile.pause_chip
        renderer._draw_state_chip(
            ctx, "PAUSED", 12, 234, font_name=label_font,
            size=chip.size, pad_x=chip.pad_x, chip_h=chip.chip_h, rise=chip.rise,
        )
    else:
        renderer._draw_text(
            ctx, "PLAYING", 11, 12, 234, 110, "left", font_name=label_font, tracking=TRACKING_MEGA
        )
    renderer._draw_text(
        ctx, "1.0.0", 11, 116, 234, width - 128, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
    )


SQUARE_PROFILE = RenderProfile(
    name="240x240",
    width=240,
    height=240,
    lyric_sizes=SQUARE_LYRIC_LAYOUT_SIZES,
    lyric_box_x=12,
    lyric_box_width=240 - 24,
    lyric_band_bottom_y=184,
    progress_geom=SQUARE_PROGRESS_GEOM,
    pause_chip=SQUARE_PAUSE_CHIP,
    paint=draw_square,
    cover=SQUARE_COVER_PLACEMENT,
)


def register(*, default: bool = True) -> RenderProfile:
    return BOARDS.register(SQUARE_PROFILE, default=default)

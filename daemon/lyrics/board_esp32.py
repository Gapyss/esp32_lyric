"""The 400x300 e-ink board (ESP32) -- geometry and full-frame painter.

Importing this module registers the 400x300 profile with :data:`BOARDS`, which
is how a process opts into serving this board family.
"""

from __future__ import annotations

from typing import Any

from .constants import (
    DISPLAY_HEIGHT,
    DISPLAY_WIDTH,
    LYRIC_BAND_BOTTOM_Y,
    LYRIC_BOX_WIDTH,
    LYRIC_BOX_X,
    MONO_FONT_NAME,
    TRACKING_MEGA,
    TRACKING_WIDE,
)
from .profiles import (
    BOARDS,
    DEFAULT_LYRIC_LAYOUT_SIZES,
    CoverPlacement,
    PauseChipGeom,
    ProgressGeom,
    RenderProfile,
    cover_placement,
)
from .state import AppState

WIDE_PROGRESS_GEOM = ProgressGeom(
    time_size=13, left_x=14, left_w=54, right_x=328, right_w=58,
    baseline_y=116, bar_x=76, bar_w=238, center_y=109, dot_r=4.5,
)
WIDE_COVER_PLACEMENT = CoverPlacement(x=324, y=32, size=60, title_w=324 - 14 - 8, frame_pad=2)
WIDE_PAUSE_CHIP = PauseChipGeom(size=13, pad_x=9, chip_h=20, rise=14, margin=14)


def draw_wide(renderer: Any, ctx: Any, state: AppState, profile: RenderProfile) -> None:
    label_font = renderer._role_font("label")
    display_font = renderer._role_font("display")
    body_font = renderer._role_font("body")
    body_medium_font = renderer._role_font("body_medium")

    renderer._draw_text(
        ctx, "NOW PLAYING", 13, 14, 18, DISPLAY_WIDTH - 28, "left", font_name=label_font, tracking=TRACKING_MEGA
    )
    renderer._draw_rule(ctx, 12, 28, DISPLAY_WIDTH - 24)

    if not state.track.title:
        # Idle: quiet centered copy in the design system's lowercase voice.
        renderer._draw_text(ctx, "nothing playing", 30, 14, 150, DISPLAY_WIDTH - 28, "center", font_name=display_font)
        renderer._draw_text(
            ctx, "waiting for youtube music", 15, 14, 182, DISPLAY_WIDTH - 28, "center", font_name=body_font
        )
        renderer._screen_rect(ctx, 14, 168, DISPLAY_WIDTH - 28, 20)
        renderer._draw_text(
            ctx, "POWERED BY CLAUDE", 13, 260, 292, 126, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
        )
        return

    place = cover_placement(profile) if state.cover is not None else None
    header_w = place.title_w if place is not None else DISPLAY_WIDTH - 28
    renderer._draw_text(ctx, state.track.title, 28, 14, 60, header_w, "left", font_name=display_font)
    renderer._draw_text(ctx, state.track.artist, 18, 14, 88, header_w, "left", font_name=body_medium_font)
    if place is not None:
        renderer._draw_cover_card(ctx, place)
    renderer._draw_progress(ctx, state, MONO_FONT_NAME, profile.progress_geom)
    renderer._draw_rule(ctx, 12, 130, DISPLAY_WIDTH - 24)

    _current, next_line, third = state.current_lines()
    current_layout = renderer._draw_lyric_band(ctx, state, profile)
    renderer._draw_text(ctx, next_line, 21, 14, 224, DISPLAY_WIDTH - 28, "center", font_name=body_font)
    if not current_layout.hide_third and third:
        renderer._draw_text(ctx, third, 16, 14, 252, DISPLAY_WIDTH - 28, "center", font_name=body_font)
        renderer._screen_rect(ctx, 14, 238, DISPLAY_WIDTH - 28, 22)
    if state.clock.paused:
        renderer._draw_state_chip(ctx, "PAUSED", 14, 292, font_name=label_font)
    else:
        renderer._draw_text(
            ctx, "PLAYING", 13, 14, 292, 120, "left", font_name=label_font, tracking=TRACKING_MEGA
        )
    renderer._draw_text(
        ctx, "1.0.0", 13, 260, 292, 126, "right", font_name=MONO_FONT_NAME, tracking=TRACKING_WIDE
    )


WIDE_PROFILE = RenderProfile(
    name="400x300",
    width=DISPLAY_WIDTH,
    height=DISPLAY_HEIGHT,
    lyric_sizes=DEFAULT_LYRIC_LAYOUT_SIZES,
    lyric_box_x=LYRIC_BOX_X,
    lyric_box_width=LYRIC_BOX_WIDTH,
    lyric_band_bottom_y=LYRIC_BAND_BOTTOM_Y,
    progress_geom=WIDE_PROGRESS_GEOM,
    pause_chip=WIDE_PAUSE_CHIP,
    paint=draw_wide,
    cover=WIDE_COVER_PLACEMENT,
)


def register(*, default: bool = True) -> RenderProfile:
    return BOARDS.register(WIDE_PROFILE, default=default)

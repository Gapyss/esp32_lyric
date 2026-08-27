#!/usr/bin/env python3
"""Backwards-compatible facade for the split :mod:`daemon.lyrics` package.

The daemon used to be one 3.5k-line module. It is now a package -- constants,
state, profiles, imaging, store, websocket, renderers, daemon, discovery and
CLI -- so the per-board layouts can be pulled apart next. This module keeps the
old flat import surface working for the test suite, ``clear_lyric_daemon.sh``
and the launchd job, and stays the daemon entry point.

New code should import from :mod:`daemon.lyrics` directly.
"""

from __future__ import annotations

# Running this file by path (``python3 daemon/lyrics_display_daemon.py``) leaves
# it outside its package, so the relative imports below fail. The launchd job and
# a decade of muscle memory both invoke it that way, so re-enter as a module
# instead of requiring everyone to switch to ``python3 -m``.
if __package__ in (None, ""):
    import pathlib as _pathlib
    import runpy as _runpy
    import sys as _bootstrap_sys

    _bootstrap_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
    _runpy.run_module("daemon.lyrics_display_daemon", run_name="__main__", alter_sys=True)
    raise SystemExit(0)

# Re-exported so tests and callers can monkeypatch them here, as they always could.
import os
import secrets
import shutil
import subprocess
import urllib.error
import urllib.parse
import urllib.request

from .lyrics.constants import (
    DISPLAY_WIDTH,
    DISPLAY_HEIGHT,
    FRAME_BYTES,
    EXTENSION_HOST,
    EXTENSION_PORT,
    BOARD_HOST,
    BOARD_PORT,
    NEGATIVE_CACHE_SECONDS,
    LRCLIB_TIMEOUT_SECONDS,
    LRCLIB_RETRY_ATTEMPTS,
    LRCLIB_RETRY_DELAY_SECONDS,
    LRCLIB_RETRYABLE_STATUS,
    LRCLIB_MAX_RETRY_AFTER_SECONDS,
    LRCLIB_USER_AGENT,
    MIN_SCHEDULE_SWAP_MS,
    ALBUM_ART_INTRO_SECONDS,
    MDNS_GROUP,
    MDNS_PORT,
    FRAME_ENVELOPE_MAGIC,
    FRAME_ENVELOPE_VERSION,
    FRAME_KIND_FULL_NOW,
    FRAME_KIND_FULL_SCHEDULED,
    FRAME_KIND_RECT_NOW,
    FRAME_KIND_RECT_SCHEDULED,
    FRAME_ENVELOPE_STRUCT,
    COLOR_ENVELOPE_MAGIC,
    COLOR_ENVELOPE_VERSION,
    COLOR_KIND_RECT_NOW,
    SEC2_MAGIC,
    SEC2_VERSION,
    SEC2_TEXT,
    SEC2_BINARY,
    SEC2_HEADER_STRUCT,
    SEC2_TAG_BYTES,
    AUTH_TIMEOUT_SECONDS,
    HEARTBEAT_INTERVAL_SECONDS,
    HEARTBEAT_TIMEOUT_SECONDS,
    BOARD_QUEUE_DEPTH,
    LYRIC_BASE_SIZE,
    LYRIC_WRAP_SIZE,
    LYRIC_MIN_SIZE,
    LYRIC_BOX_X,
    LYRIC_BOX_WIDTH,
    LYRIC_BAND_TOP_Y,
    LYRIC_BAND_BOTTOM_Y,
    LYRIC_SINGLE_BASELINE_Y,
    LYRIC_SINGLE_HIGHLIGHT_Y,
    LYRIC_HIGHLIGHT_GAP_Y,
    BRAND_FONT_NAMES,
    MONO_FONT_NAME,
    TRACKING_MEGA,
    TRACKING_WIDE,
    KARAOKE_STROKE_PCT,
    LyricBreakFn,
)
from .lyrics.security import (
    DaemonIdentity,
    default_identity_path,
    load_or_create_identity,
    _auth_transcript,
    derive_session_keys,
    monotonic_ms,
    clamp,
)
from .lyrics.state import (
    TrackInfo,
    CoverArt,
    PlaybackClock,
    Lyrics,
    AppState,
    ScheduledFrame,
    DirtyRect,
    ColorRect,
)
from .lyrics.profiles import (
    BOARDS,
    BoardRegistry,
    LyricLayoutSizes,
    LyricLayout,
    DEFAULT_LYRIC_LAYOUT_SIZES,
    RenderProfile,
    ProgressGeom,
    CoverPlacement,
    PauseChipGeom,
    cover_placement,
    max_cover_size,
    hero_cover_size,
)

# Importing this facade registers both board families, which is what the single
# combined daemon always served. ``serve --boards`` narrows it per process.
from .lyrics.board_esp32 import WIDE_PROFILE, WIDE_PROGRESS_GEOM
from .lyrics.board_esp8266 import (
    SQUARE_LYRIC_LAYOUT_SIZES,
    SQUARE_PROFILE,
    SQUARE_PROGRESS_GEOM,
)
from .lyrics.cli import select_boards

select_boards(("esp32", "esp8266"))


def _legacy_profile_view(name: str) -> object:
    """The three profile lookups that used to be module-level constants.

    They are derived from the live registry rather than frozen at import,
    because ``serve --boards`` narrows it per process: a frozen ``DEFAULT_PROFILE``
    would still claim 400x300 inside the ESP8266 daemon.
    """
    if name == "DEFAULT_PROFILE":
        return BOARDS.default
    if name == "PROFILES_BY_SIZE":
        return {(p.width, p.height): p for p in BOARDS.all()}
    if name == "COVER_PLACEMENTS":
        return {p.name: p.cover for p in BOARDS.all() if p.cover is not None}
    raise AttributeError(name)
from .lyrics.imaging import (
    blit_cover_centered,
    composite_chip,
    COVER_ALLOWED_DOMAINS,
    COVER_FETCH_TIMEOUT_SECONDS,
    COVER_MAX_BYTES,
    cover_host_allowed,
    cover_url_for,
    fetch_cover_bytes,
    floyd_steinberg_1bit,
    blit_cover,
    color_cover_rect,
    composite_rgb565_chip,
    profile_from_board_path,
    board_wants_rgb565,
    fit_lyric_layout,
    _layout_break_rows,
    _collapse_to_two_rows,
    _lyric_layout,
    _lyric_baselines,
    dirty_rect,
    make_frame_envelope,
    make_color_envelope,
    format_time,
    invert_frame,
    pack_1bpp,
    set_pixel,
    fill_rect,
    draw_rect,
)
from .lyrics.store import (
    LyricsStore,
    row_to_lyrics,
    normalized_lyrics_cache_part,
    LEGACY_CACHE_FEAT_RE,
    LEGACY_CACHE_TITLE_NOISE_RE,
    LEGACY_CACHE_MATCH_PUNCT_RE,
    legacy_lyrics_cache_key,
    canonical_lyrics_cache_title,
    legacy_cache_row_to_lyrics,
    lyrics_to_storage_text,
    parse_lrc,
    parse_lrc_timestamp,
    strip_lrc_metadata,
    parse_lrc_with_syllables,
    parse_inline_syllables,
    lyrics_from_lrclib_payload,
    ResolveCancelled,
    _is_timeout_error,
    _is_retryable_error,
    _retry_delay_seconds,
    lrclib_request,
    fetch_lrclib,
)
from .lyrics.wsproto import (
    WebSocketConnection,
)
from .lyrics.render import (
    FrameRenderer,
    CoreTextFrameRenderer,
    FallbackFrameRenderer,
)
from .lyrics.daemon import (
    LyricsDisplayDaemon,
)
from .lyrics.discovery import (
    mdns_encode_name,
    mdns_read_name,
    mdns_query_names,
    mdns_rr,
    local_ipv4,
    DnsSdAdvertiser,
)
from .lyrics.cli import (
    build_renderer,
    default_db_path,
    track_from_import_args,
    import_lrc_command,
    cache_command,
    run,
    main,
)


# ---------------------------------------------------------------------------
# Assignment pass-through.
#
# While this was one module, ``lyrics_display_daemon.fetch_lrclib = fake`` was
# seen by every caller, because there was only one namespace. After the split a
# name like ``lrclib_request`` lives in ``lyrics.store`` and is bound by value
# into whichever modules import it, so a plain rebind here would land on the
# facade only and silently do nothing. Forward each assignment to every package
# module that holds the same name, which restores the old single-namespace
# behaviour for tests and for anything else that patches this module.
# ---------------------------------------------------------------------------

import sys as _sys
import types as _types

from .lyrics import (
    cli as _cli, constants as _constants, daemon as _daemon, discovery as _discovery,
    imaging as _imaging, profiles as _profiles, render as _render, security as _security,
    state as _state, store as _store, wsproto as _wsproto,
)

_PACKAGE_MODULES = (
    _constants, _security, _state, _profiles, _imaging, _store, _wsproto,
    _render, _daemon, _discovery, _cli,
)


class _Facade(_types.ModuleType):
    """Module type that mirrors attribute writes into the package."""

    def __setattr__(self, name: str, value: object) -> None:
        super().__setattr__(name, value)
        if name.startswith("_"):
            return
        for module in _PACKAGE_MODULES:
            if name in vars(module):
                setattr(module, name, value)

    def __getattr__(self, name: str) -> object:
        # Only reached for names not bound on the module, i.e. the registry-
        # derived legacy lookups above.
        return _legacy_profile_view(name)

    def __delattr__(self, name: str) -> None:
        super().__delattr__(name)
        for module in _PACKAGE_MODULES:
            vars(module).pop(name, None)


_sys.modules[__name__].__class__ = _Facade


if __name__ == "__main__":
    raise SystemExit(main())

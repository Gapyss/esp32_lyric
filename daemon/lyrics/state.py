"""Playback state: track, clock, lyrics and the frame/rect value objects."""

from __future__ import annotations

from dataclasses import dataclass, field

from .constants import ALBUM_ART_INTRO_SECONDS
from .security import clamp, monotonic_ms

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

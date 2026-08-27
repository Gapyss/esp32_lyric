"""The daemon: owns playback state, renders frames and drives board sockets."""

from __future__ import annotations

import asyncio
import hmac
import hashlib
import json
import re
import secrets
import threading
from dataclasses import field
from typing import Any
from urllib.parse import parse_qs, urlparse

from .constants import ALBUM_ART_INTRO_SECONDS, AUTH_TIMEOUT_SECONDS, FRAME_KIND_FULL_NOW, FRAME_KIND_FULL_SCHEDULED, FRAME_KIND_RECT_NOW, MIN_SCHEDULE_SWAP_MS
from .security import DaemonIdentity, _auth_transcript, derive_session_keys, monotonic_ms
from .state import AppState, ColorRect, CoverArt, Lyrics, ScheduledFrame, TrackInfo
from .profiles import BOARDS, RenderProfile, cover_placement, hero_cover_size, max_cover_size
from .imaging import blit_cover, blit_cover_centered, board_wants_rgb565, color_cover_rect, composite_chip, composite_rgb565_chip, cover_url_for, dirty_rect, fetch_cover_bytes, invert_frame, make_color_envelope, make_frame_envelope, profile_from_board_path
from .store import LyricsStore, ResolveCancelled, _is_timeout_error, fetch_lrclib
from .relay import RELAY_PATH, RelayHub
from .wsproto import WebSocketConnection
from .render import FrameRenderer

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
        # Downstream daemons (e.g. the ESP8266 process) mirroring this one's
        # extension feed. Empty in a process that has no subscribers.
        self.relay = RelayHub()

    def _cancel_inflight_resolve(self) -> None:
        if self._resolve_cancel is not None:
            self._resolve_cancel.set()
            self._resolve_cancel = None

    async def handle_extension(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        conn = WebSocketConnection(reader, writer)
        try:
            path = await conn.handshake()
            if path == RELAY_PATH:
                await self.handle_relay_subscriber(conn)
                return
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

    async def handle_relay_subscriber(self, conn: WebSocketConnection) -> None:
        """Serve a downstream daemon mirroring this one's extension feed."""
        self.relay.add(conn)
        print("relay subscriber connected")
        try:
            await conn.send_text(await self.relay_snapshot())
            while await conn.recv() is not None:
                pass  # downstream is read-only; drain so control frames work
        finally:
            self.relay.discard(conn)
            conn.close()
            print("relay subscriber disconnected")

    async def relay_snapshot(self) -> dict[str, Any]:
        """Current playback state, for a downstream that joined mid-song.

        Positions here are already normalised against this daemon's media
        timeline offset, so the downstream applies them with its own offset
        cleared rather than through the usual tick path -- normalising twice
        would shift the lyric timing by the length of every track played so far.
        """
        async with self.state_lock:
            track = self.state.track
            clock = self.state.clock
            return {
                "type": "relay-snapshot",
                "payload": {
                    "videoId": track.video_id, "title": track.title,
                    "artist": track.artist, "album": track.album,
                    "durationSec": track.duration_sec, "artUrl": track.art_url,
                    "positionSec": clock.interpolated_position(),
                    "paused": clock.paused, "playbackRate": clock.playback_rate,
                    "theme": self.theme,
                },
            }

    async def apply_relay_snapshot(self, payload: dict[str, Any]) -> None:
        """Adopt an upstream snapshot as this daemon's whole playback state."""
        await self.set_theme(str(payload.get("theme") or ""))
        await self.set_track(TrackInfo(
            video_id=str(payload.get("videoId") or ""),
            title=str(payload.get("title") or ""),
            artist=str(payload.get("artist") or ""),
            album=str(payload.get("album") or ""),
            duration_sec=float(payload.get("durationSec") or 0),
            art_url=str(payload.get("artUrl") or ""),
        ))
        self.media_timeline_offset_sec = 0.0
        await self.update_clock(payload)
        await self.render_and_broadcast()

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
        await self.relay.broadcast(raw)
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
        return self.last_frames.get(BOARDS.default.name)

    @last_frame.setter
    def last_frame(self, frame: bytes | None) -> None:
        if frame is None:
            self.last_frames.pop(BOARDS.default.name, None)
        else:
            self.last_frames[BOARDS.default.name] = frame

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
        margin = profile.pause_chip.margin
        frame = composite_chip(
            frame, value, chip_mask, art_x + margin, art_y + margin, chip_w, chip_h, profile
        )
        return frame

    def _active_profiles(self) -> list[RenderProfile]:
        profiles: dict[str, RenderProfile] = {conn.profile.name: conn.profile for conn in self.boards}
        if not profiles:
            profiles[BOARDS.default.name] = BOARDS.default
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
            margin = profile.pause_chip.margin
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

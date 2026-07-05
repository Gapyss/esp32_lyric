from __future__ import annotations

import asyncio
import tempfile
import threading
import unittest
from pathlib import Path

import daemon.lyrics_display_daemon as lyrics_display_daemon
from daemon.lyrics_display_daemon import (
    AppState,
    FallbackFrameRenderer,
    LyricLayoutSizes,
    Lyrics,
    LyricsDisplayDaemon,
    LyricsStore,
    TrackInfo,
    WebSocketConnection,
    dirty_rect,
    fit_lyric_layout,
    invert_frame,
    parse_lrc_with_syllables,
)


class FakeWriter:
    def __init__(self) -> None:
        self.writes: list[bytes] = []
        self.closed = False

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    async def drain(self) -> None:
        await asyncio.sleep(0)

    def close(self) -> None:
        self.closed = True


def masked_client_frame(opcode: int, payload: bytes = b"", *, fin: bool = True) -> bytes:
    first = (0x80 if fin else 0) | opcode
    mask = b"abcd"
    length = len(payload)
    if length < 126:
        header = bytes([first, 0x80 | length])
    elif length < 65536:
        header = bytes([first, 0x80 | 126]) + length.to_bytes(2, "big")
    else:
        header = bytes([first, 0x80 | 127]) + length.to_bytes(8, "big")
    masked = bytes(byte ^ mask[idx % 4] for idx, byte in enumerate(payload))
    return header + mask + masked


def server_frame_payloads(data: bytes) -> list[tuple[int, bytes]]:
    frames: list[tuple[int, bytes]] = []
    cursor = 0
    while cursor < len(data):
        opcode = data[cursor] & 0x0F
        length = data[cursor + 1] & 0x7F
        cursor += 2
        if length == 126:
            length = int.from_bytes(data[cursor : cursor + 2], "big")
            cursor += 2
        elif length == 127:
            length = int.from_bytes(data[cursor : cursor + 8], "big")
            cursor += 8
        payload = data[cursor : cursor + length]
        cursor += length
        frames.append((opcode, payload))
    return frames


def parse_frame_envelope(payload: bytes) -> dict[str, object]:
    header_len = lyrics_display_daemon.FRAME_ENVELOPE_STRUCT.size
    (
        magic,
        version,
        kind,
        width,
        height,
        x,
        y,
        rect_width,
        rect_height,
        row_bytes,
        swap_in_ms,
        payload_len,
    ) = lyrics_display_daemon.FRAME_ENVELOPE_STRUCT.unpack(payload[:header_len])
    body = payload[header_len:]
    return {
        "magic": magic,
        "version": version,
        "kind": kind,
        "width": width,
        "height": height,
        "x": x,
        "y": y,
        "rect_width": rect_width,
        "rect_height": rect_height,
        "row_bytes": row_bytes,
        "swap_in_ms": swap_in_ms,
        "payload_len": payload_len,
        "payload": body,
    }


class LyricsStoreTests(unittest.TestCase):
    def test_store_can_be_used_from_worker_thread(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            track = TrackInfo(video_id="video-1", title="Song", artist="Artist")
            store.save_lrclib(track, Lyrics(plain=["line one"], resolved=True))

            result = asyncio.run(asyncio.to_thread(store.manual_or_cached, track))

        self.assertIsNotNone(result)
        self.assertEqual(result.plain, ["line one"])
        self.assertTrue(result.resolved)

    def test_store_reads_legacy_clawdmeter_cache_exact_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            store.conn.execute(
                "CREATE TABLE lyrics_cache ("
                "key TEXT PRIMARY KEY, track TEXT, artist TEXT, duration REAL, "
                "synced_lyrics TEXT, plain_lyrics TEXT, instrumental INTEGER, updated REAL)"
            )
            store.conn.execute(
                "INSERT INTO lyrics_cache "
                "(key, track, artist, duration, synced_lyrics, plain_lyrics, instrumental, updated) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                ("ความจริง\x1froom39\x1f252", "ความจริง", "Room39", 252, None, "line one\nline two", 0, 0),
            )
            store.conn.commit()

            result = store.manual_or_cached(
                TrackInfo(video_id="video-1", title="ความจริง", artist="Room39", duration_sec=252)
            )

        self.assertIsNotNone(result)
        self.assertEqual(result.plain, ["line one", "line two"])
        self.assertTrue(result.resolved)

    def test_store_reads_legacy_clawdmeter_cache_by_title_and_duration(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            store.conn.execute(
                "CREATE TABLE lyrics_cache ("
                "key TEXT PRIMARY KEY, track TEXT, artist TEXT, duration REAL, "
                "synced_lyrics TEXT, plain_lyrics TEXT, instrumental INTEGER, updated REAL)"
            )
            store.conn.execute(
                "INSERT INTO lyrics_cache "
                "(key, track, artist, duration, synced_lyrics, plain_lyrics, instrumental, updated) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    "have you ever seen the rain?\x1fcreedence clearwater revival\x1f159",
                    "Have You Ever Seen The Rain?",
                    "Creedence Clearwater Revival",
                    159,
                    "[00:01.00]line one",
                    None,
                    0,
                    0,
                ),
            )
            store.conn.commit()

            result = store.manual_or_cached(
                TrackInfo(
                    video_id="video-1",
                    title="Have You Ever Seen The Rain",
                    artist="",
                    duration_sec=160,
                )
            )

        self.assertIsNotNone(result)
        self.assertEqual(result.synced, [(1000, "line one")])
        self.assertTrue(result.resolved)

    def test_worker_resolution_fetches_and_caches_lyrics(self) -> None:
        async def run_resolution(store: LyricsStore, track: TrackInfo) -> Lyrics:
            daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())
            return await asyncio.to_thread(daemon._resolve_track_sync, track)

        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            track = TrackInfo(video_id="video-1", title="Song", artist="Artist")
            original_fetch = lyrics_display_daemon.fetch_lrclib
            lyrics_display_daemon.fetch_lrclib = lambda _track: Lyrics(plain=["fetched"], resolved=True)
            try:
                result = asyncio.run(run_resolution(store, track))
            finally:
                lyrics_display_daemon.fetch_lrclib = original_fetch

            cached = store.manual_or_cached(track)

        self.assertEqual(result.plain, ["fetched"])
        self.assertIsNotNone(cached)
        self.assertEqual(cached.plain, ["fetched"])

    def test_negative_cache_row_is_resolved_without_lines(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            track = TrackInfo(video_id="video-1", title="Song", artist="Artist")
            store.save_lrclib(track, Lyrics(resolved=True))

            cached = store.manual_or_cached(track)

        self.assertIsNotNone(cached)
        self.assertTrue(cached.resolved)
        self.assertEqual(cached.plain, [])
        self.assertEqual(cached.synced, [])

    def test_clear_cache_can_delete_only_lrclib_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
            track = TrackInfo(video_id="video-1", title="Song", artist="Artist")
            store.save_manual(track, Lyrics(plain=["manual"], resolved=True))
            store.save_lrclib(track, Lyrics(plain=["cached"], resolved=True))

            deleted = store.clear_cache(source="lrclib")
            cached = store.manual_or_cached(track)

        self.assertEqual(deleted, 1)
        self.assertIsNotNone(cached)
        self.assertEqual(cached.plain, ["manual"])

    def test_set_track_reads_cache_off_event_loop_thread(self) -> None:
        async def run() -> tuple[int, int]:
            with tempfile.TemporaryDirectory() as tmpdir:
                store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
                loop_thread = threading.get_ident()
                cache_thread = 0

                def manual_or_cached(_track: TrackInfo) -> Lyrics:
                    nonlocal cache_thread
                    cache_thread = threading.get_ident()
                    return Lyrics(plain=["cached"], resolved=True)

                store.manual_or_cached = manual_or_cached  # type: ignore[method-assign]
                daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())
                await daemon.set_track(TrackInfo(video_id="video-1", title="Song", artist="Artist"))
                return loop_thread, cache_thread

        loop_thread, cache_thread = asyncio.run(run())

        self.assertNotEqual(cache_thread, 0)
        self.assertNotEqual(cache_thread, loop_thread)


class LrclibTests(unittest.TestCase):
    def test_search_falls_back_to_plain_candidate_when_no_synced_match_exists(self) -> None:
        calls: list[str] = []

        def fake_request(path: str, _params: dict[str, object]) -> object:
            calls.append(path)
            if path == "/api/get":
                raise lyrics_display_daemon.urllib.error.HTTPError(
                    url="https://lrclib.net/api/get",
                    code=404,
                    msg="not found",
                    hdrs=None,
                    fp=None,
                )
            return [
                {"duration": 120, "plainLyrics": "plain one\nplain two", "syncedLyrics": ""},
                {"duration": 130, "plainLyrics": "too far", "syncedLyrics": ""},
            ]

        original_request = lyrics_display_daemon.lrclib_request
        lyrics_display_daemon.lrclib_request = fake_request
        try:
            result = lyrics_display_daemon.fetch_lrclib(
                TrackInfo(video_id="video-1", title="Song", artist="Artist", duration_sec=120)
            )
        finally:
            lyrics_display_daemon.lrclib_request = original_request

        self.assertEqual(calls, ["/api/get", "/api/search"])
        self.assertEqual(result.synced, [])
        self.assertEqual(result.plain, ["plain one", "plain two"])

    def test_search_prefers_synced_candidate_over_plain_candidate(self) -> None:
        def fake_request(path: str, _params: dict[str, object]) -> object:
            if path == "/api/get":
                raise lyrics_display_daemon.urllib.error.HTTPError(
                    url="https://lrclib.net/api/get",
                    code=404,
                    msg="not found",
                    hdrs=None,
                    fp=None,
                )
            return [
                {"duration": 120, "plainLyrics": "plain", "syncedLyrics": ""},
                {"duration": 121, "plainLyrics": "synced plain", "syncedLyrics": "[00:01.00]synced"},
            ]

        original_request = lyrics_display_daemon.lrclib_request
        lyrics_display_daemon.lrclib_request = fake_request
        try:
            result = lyrics_display_daemon.fetch_lrclib(
                TrackInfo(video_id="video-1", title="Song", artist="Artist", duration_sec=120)
            )
        finally:
            lyrics_display_daemon.lrclib_request = original_request

        self.assertEqual(result.synced, [(1000, "synced")])
        self.assertEqual(result.plain, ["synced plain"])


class AppStateTests(unittest.TestCase):
    def test_negative_cache_falls_back_to_now_playing_area(self) -> None:
        state = AppState(
            track=TrackInfo(video_id="video-1", title="Song", artist="Artist"),
            lyrics=Lyrics(resolved=True),
            resolving_key="",
        )

        self.assertEqual(state.current_lines(), ("", "", ""))

    def test_unresolved_track_shows_resolving(self) -> None:
        state = AppState(
            track=TrackInfo(video_id="video-1", title="Song", artist="Artist"),
            lyrics=Lyrics(),
            resolving_key="video-1",
        )

        self.assertEqual(state.current_lines(), ("Resolving lyrics...", "", ""))

    def test_enhanced_lrc_syllables_drive_highlight_fraction(self) -> None:
        synced, syllables = parse_lrc_with_syllables("[00:10.00]<00:10.00>hel <00:10.50>lo\n[00:12.00]next")
        state = AppState(
            track=TrackInfo(video_id="video-1", title="Song", artist="Artist"),
            lyrics=Lyrics(synced=synced, syllables=syllables, resolved=True),
        )
        state.clock.update(position_sec=10.6, paused=True, playback_rate=1)

        self.assertEqual(state.current_lines()[0], "hel lo")
        self.assertEqual(state.current_line_highlight_fraction(), 1.0)

    def test_synced_lyrics_are_blank_before_first_timestamp(self) -> None:
        state = AppState(
            track=TrackInfo(video_id="video-1", title="Song", artist="Artist"),
            lyrics=Lyrics(synced=[(10_000, "first"), (12_000, "second")], resolved=True),
        )
        state.clock.update(position_sec=5, paused=True, playback_rate=1)

        self.assertEqual(state.active_line_index(), -1)
        self.assertEqual(state.current_lines(), ("", "", ""))
        self.assertEqual(state.current_line_highlight_fraction(), 0.0)


class ExtensionPlaybackTests(unittest.TestCase):
    def test_autoplay_tick_normalizes_cumulative_video_timeline_after_ended(self) -> None:
        async def run() -> tuple[float, float]:
            with tempfile.TemporaryDirectory() as tmpdir:
                store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
                store.save_lrclib(TrackInfo(video_id="video-1"), Lyrics(resolved=True))
                store.save_lrclib(TrackInfo(video_id="video-2"), Lyrics(resolved=True))
                daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())

                await daemon.handle_extension_message(
                    '{"type":"now-playing","payload":'
                    '{"videoId":"video-1","title":"First","artist":"Artist","durationSec":180}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"tick","payload":'
                    '{"positionSec":179,"durationSec":180,"paused":false,"playbackRate":1}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"event","payload":'
                    '{"type":"ended","positionSec":180,"durationSec":400,"paused":true,"playbackRate":1}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"now-playing","payload":'
                    '{"videoId":"video-2","title":"Second","artist":"Artist","durationSec":400}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"tick","payload":'
                    '{"positionSec":181,"durationSec":400,"paused":false,"playbackRate":1}}'
                )

                return daemon.state.clock.position_sec, daemon.state.track.duration_sec

        position_sec, duration_sec = asyncio.run(run())

        self.assertEqual(position_sec, 1)
        self.assertEqual(duration_sec, 220)

    def test_per_track_tick_resets_autoplay_offset_without_subtracting_duration(self) -> None:
        async def run() -> tuple[float, float]:
            with tempfile.TemporaryDirectory() as tmpdir:
                store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
                store.save_lrclib(TrackInfo(video_id="video-1"), Lyrics(resolved=True))
                store.save_lrclib(TrackInfo(video_id="video-2"), Lyrics(resolved=True))
                daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())

                await daemon.handle_extension_message(
                    '{"type":"now-playing","payload":'
                    '{"videoId":"video-1","title":"First","artist":"Artist","durationSec":180}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"event","payload":'
                    '{"type":"ended","positionSec":180,"durationSec":180,"paused":true,"playbackRate":1}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"now-playing","payload":'
                    '{"videoId":"video-2","title":"Second","artist":"Artist","durationSec":220}}'
                )
                await daemon.handle_extension_message(
                    '{"type":"tick","payload":'
                    '{"positionSec":1,"durationSec":220,"paused":false,"playbackRate":1}}'
                )

                return daemon.state.clock.position_sec, daemon.state.track.duration_sec

        position_sec, duration_sec = asyncio.run(run())

        self.assertEqual(position_sec, 1)
        self.assertEqual(duration_sec, 220)


class ThemeTests(unittest.TestCase):
    def test_invert_frame_flips_every_bit(self) -> None:
        self.assertEqual(invert_frame(b"\x00\xff\x0f"), b"\xff\x00\xf0")

    def test_set_theme_message_inverts_rendered_frame(self) -> None:
        async def run() -> tuple[bytes, bytes, str]:
            with tempfile.TemporaryDirectory() as tmpdir:
                store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
                daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())

                await daemon.handle_extension_message('{"type":"tick","payload":{"positionSec":1}}')
                dark_frame = daemon.last_frame
                await daemon.handle_extension_message(
                    '{"type":"set-theme","payload":{"theme":"light"}}'
                )
                light_frame = daemon.last_frame

                return dark_frame, light_frame, daemon.theme

        dark_frame, light_frame, theme = asyncio.run(run())

        self.assertEqual(theme, "light")
        self.assertEqual(light_frame, invert_frame(dark_frame))

    def test_unknown_theme_value_is_ignored(self) -> None:
        async def run() -> str:
            with tempfile.TemporaryDirectory() as tmpdir:
                store = LyricsStore(Path(tmpdir) / "lyrics.sqlite3")
                daemon = LyricsDisplayDaemon(store, FallbackFrameRenderer())
                await daemon.handle_extension_message(
                    '{"type":"set-theme","payload":{"theme":"neon"}}'
                )
                return daemon.theme

        self.assertEqual(asyncio.run(run()), "dark")


class LyricLayoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sizes = LyricLayoutSizes()
        self.box_w = lyrics_display_daemon.LYRIC_BOX_WIDTH
        self.box_h = lyrics_display_daemon.LYRIC_BAND_BOTTOM_Y - lyrics_display_daemon.LYRIC_BAND_TOP_Y

    def test_short_line_stays_at_base_size(self) -> None:
        layout = fit_lyric_layout("short", self.box_w, self.box_h, self.sizes, lambda text, _size, _width: [text])

        self.assertEqual(layout.font_size, 30)
        self.assertEqual(layout.rows, ["short"])
        self.assertEqual(layout.baselines, [178])
        self.assertEqual(layout.bottom_row_baseline_y, 178)
        self.assertEqual(layout.highlight_y, 198)
        self.assertFalse(layout.hide_third)

    def test_mildly_long_line_shrinks_before_wrapping(self) -> None:
        def fake_break(text: str, size: float, _width: int) -> list[str]:
            return [text] if size <= 26 else ["too", "wide"]

        layout = fit_lyric_layout("mild", self.box_w, self.box_h, self.sizes, fake_break)

        self.assertEqual(layout.font_size, 26)
        self.assertEqual(layout.rows, ["mild"])
        self.assertEqual(layout.baselines, [178])
        self.assertFalse(layout.hide_third)

    def test_long_line_wraps_to_two_rows_at_wrap_size(self) -> None:
        def fake_break(_text: str, size: float, _width: int) -> list[str]:
            return ["first row", "second row"] if size <= 26 else ["one", "two", "three"]

        layout = fit_lyric_layout("long", self.box_w, self.box_h, self.sizes, fake_break)

        self.assertEqual(layout.font_size, 26)
        self.assertEqual(layout.rows, ["first row", "second row"])
        self.assertEqual(layout.baselines, [156, 188])
        self.assertEqual(layout.bottom_row_baseline_y, 188)
        self.assertEqual(layout.highlight_y, 204)
        self.assertTrue(layout.hide_third)

    def test_pathological_line_shrinks_two_rows_toward_floor(self) -> None:
        def fake_break(_text: str, size: float, _width: int) -> list[str]:
            return ["first row", "second row"] if size <= 14 else ["one", "two", "three"]

        layout = fit_lyric_layout("pathological", self.box_w, self.box_h, self.sizes, fake_break)

        self.assertEqual(layout.font_size, 14)
        self.assertEqual(layout.rows, ["first row", "second row"])
        self.assertEqual(layout.baselines, [160, 178])
        self.assertEqual(layout.bottom_row_baseline_y, 178)
        self.assertTrue(layout.hide_third)

    def test_pathological_floor_keeps_overflow_text_in_second_row(self) -> None:
        def fake_break(_text: str, _size: float, _width: int) -> list[str]:
            return ["one", "two", "three"]

        layout = fit_lyric_layout("pathological", self.box_w, self.box_h, self.sizes, fake_break)

        self.assertEqual(layout.font_size, 14)
        self.assertEqual(layout.rows, ["one", "two three"])
        self.assertTrue(layout.hide_third)


class DirtyRectTests(unittest.TestCase):
    def test_dirty_rect_is_byte_aligned(self) -> None:
        before = bytes(lyrics_display_daemon.FRAME_BYTES)
        after = bytearray(before)
        after[10 * (lyrics_display_daemon.DISPLAY_WIDTH // 8) + 3] = 0x80

        rect = dirty_rect(before, bytes(after))

        self.assertIsNotNone(rect)
        assert rect is not None
        self.assertEqual((rect.x, rect.y, rect.width, rect.height, rect.row_bytes), (24, 10, 8, 1, 1))
        self.assertEqual(rect.payload, b"\x80")


class WebSocketConnectionTests(unittest.TestCase):
    def test_recv_handles_many_pings_without_recursion(self) -> None:
        async def run() -> tuple[tuple[int, bytes] | None, bytes]:
            reader = asyncio.StreamReader()
            writer = FakeWriter()
            conn = WebSocketConnection(reader, writer)  # type: ignore[arg-type]
            reader.feed_data(masked_client_frame(9, b"x") * 1100 + masked_client_frame(1, b"ready"))
            message = await conn.recv()
            return message, b"".join(writer.writes)

        message, writes = asyncio.run(run())

        self.assertEqual(message, (1, b"ready"))
        self.assertEqual(len(server_frame_payloads(writes)), 1100)
        self.assertTrue(all(opcode == 10 for opcode, _payload in server_frame_payloads(writes)))

    def test_recv_reassembles_fragmented_text_around_ping(self) -> None:
        async def run() -> tuple[int, bytes] | None:
            reader = asyncio.StreamReader()
            conn = WebSocketConnection(reader, FakeWriter())  # type: ignore[arg-type]
            reader.feed_data(
                masked_client_frame(1, b"he", fin=False)
                + masked_client_frame(9, b"x")
                + masked_client_frame(0, b"llo")
            )
            return await conn.recv()

        self.assertEqual(asyncio.run(run()), (1, b"hello"))

    def test_recv_rejects_unmasked_client_frame(self) -> None:
        async def run() -> None:
            reader = asyncio.StreamReader()
            conn = WebSocketConnection(reader, FakeWriter())  # type: ignore[arg-type]
            reader.feed_data(b"\x81\x02hi")
            await conn.recv()

        with self.assertRaises(ValueError):
            asyncio.run(run())

    def test_send_frame_uses_single_binary_envelope(self) -> None:
        async def run() -> list[tuple[int, bytes]]:
            conn = WebSocketConnection(asyncio.StreamReader(), FakeWriter())  # type: ignore[arg-type]
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(LyricsStore(Path(tmpdir) / "lyrics.sqlite3"), FallbackFrameRenderer())
                await daemon.send_frame(conn, bytes(lyrics_display_daemon.FRAME_BYTES), now=True, swap_in_ms=0)
                return server_frame_payloads(b"".join(conn.writer.writes))

        frames = asyncio.run(run())
        envelope = parse_frame_envelope(frames[0][1])

        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][0], 2)
        self.assertEqual(envelope["magic"], lyrics_display_daemon.FRAME_ENVELOPE_MAGIC)
        self.assertEqual(envelope["kind"], lyrics_display_daemon.FRAME_KIND_FULL_NOW)
        self.assertEqual(envelope["payload_len"], lyrics_display_daemon.FRAME_BYTES)

    def test_board_handshake_auth_accepts_query_token(self) -> None:
        async def run() -> tuple[bool, bool, bool]:
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(
                    LyricsStore(Path(tmpdir) / "lyrics.sqlite3"),
                    FallbackFrameRenderer(),
                    board_token="secret",
                )
                return (
                    daemon.board_handshake_allowed("/board?token=secret", {}),
                    daemon.board_handshake_allowed("/board?token=wrong", {}),
                    daemon.board_handshake_allowed("/extension?token=secret", {}),
                )

        accepted, wrong_token, wrong_path = asyncio.run(run())
        self.assertTrue(accepted)
        self.assertFalse(wrong_token)
        self.assertFalse(wrong_path)


class SchedulerTests(unittest.TestCase):
    def test_synced_playback_queues_next_line_frame_with_swap_delay(self) -> None:
        async def run() -> list[dict[str, object]]:
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(LyricsStore(Path(tmpdir) / "lyrics.sqlite3"), FallbackFrameRenderer())
                conn = WebSocketConnection(asyncio.StreamReader(), FakeWriter())  # type: ignore[arg-type]
                daemon.boards.add(conn)
                daemon.state = AppState(
                    track=TrackInfo(video_id="video-1", title="Song", artist="Artist", duration_sec=30),
                    lyrics=Lyrics(synced=[(0, "first"), (10_000, "second")], resolved=True),
                )
                daemon.state.clock.update(position_sec=5, paused=False, playback_rate=1)

                await daemon.render_and_broadcast()
                if daemon.scheduled_task is not None:
                    daemon.scheduled_task.cancel()

                frames = server_frame_payloads(b"".join(conn.writer.writes))
                return [parse_frame_envelope(payload) for opcode, payload in frames if opcode == 2]

        envelopes = asyncio.run(run())

        self.assertEqual(envelopes[0]["kind"], lyrics_display_daemon.FRAME_KIND_FULL_NOW)
        self.assertEqual(envelopes[0]["swap_in_ms"], 0)
        self.assertEqual(envelopes[1]["kind"], lyrics_display_daemon.FRAME_KIND_FULL_SCHEDULED)
        self.assertGreaterEqual(envelopes[1]["swap_in_ms"], 4500)
        self.assertLessEqual(envelopes[1]["swap_in_ms"], 5500)

    def test_paused_synced_playback_does_not_queue_next_line(self) -> None:
        async def run() -> list[dict[str, object]]:
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(LyricsStore(Path(tmpdir) / "lyrics.sqlite3"), FallbackFrameRenderer())
                conn = WebSocketConnection(asyncio.StreamReader(), FakeWriter())  # type: ignore[arg-type]
                daemon.boards.add(conn)
                daemon.state = AppState(
                    track=TrackInfo(video_id="video-1", title="Song", artist="Artist", duration_sec=30),
                    lyrics=Lyrics(synced=[(0, "first"), (10_000, "second")], resolved=True),
                )
                daemon.state.clock.update(position_sec=5, paused=True, playback_rate=1)

                await daemon.render_and_broadcast()

                frames = server_frame_payloads(b"".join(conn.writer.writes))
                return [parse_frame_envelope(payload) for opcode, payload in frames if opcode == 2]

        envelopes = asyncio.run(run())

        self.assertEqual([envelope["kind"] for envelope in envelopes], [lyrics_display_daemon.FRAME_KIND_FULL_NOW])

    def test_immediate_second_frame_uses_dirty_rect(self) -> None:
        async def run() -> list[tuple[int, bytes]]:
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(LyricsStore(Path(tmpdir) / "lyrics.sqlite3"), FallbackFrameRenderer())
                conn = WebSocketConnection(asyncio.StreamReader(), FakeWriter())  # type: ignore[arg-type]
                frame1 = bytes(lyrics_display_daemon.FRAME_BYTES)
                frame2 = bytearray(frame1)
                frame2[0] = 1
                await daemon.send_frame(conn, frame1, now=True, swap_in_ms=0)
                await daemon.send_frame(conn, bytes(frame2), now=True, swap_in_ms=0)
                return server_frame_payloads(b"".join(conn.writer.writes))

        frames = asyncio.run(run())
        envelopes = [parse_frame_envelope(payload) for opcode, payload in frames if opcode == 2]

        self.assertEqual(envelopes[0]["kind"], lyrics_display_daemon.FRAME_KIND_FULL_NOW)
        self.assertEqual(envelopes[1]["kind"], lyrics_display_daemon.FRAME_KIND_RECT_NOW)
        self.assertEqual(envelopes[1]["payload_len"], 1)

    def test_scheduled_swap_invalidates_board_dirty_rect_base(self) -> None:
        # Once a scheduled swap becomes due the board may hold any of several
        # frames (the firmware can skip a pending frame), so the daemon must not
        # keep diffing against the pre-swap frame. It invalidates the base so the
        # next now-frame is sent full and resyncs the board -- otherwise a tight
        # rect against a stale base leaves ghost pixels (e.g. long -> short line).
        async def run() -> tuple[bytes | None, int]:
            with tempfile.TemporaryDirectory() as tmpdir:
                daemon = LyricsDisplayDaemon(LyricsStore(Path(tmpdir) / "lyrics.sqlite3"), FallbackFrameRenderer())
                conn = WebSocketConnection(asyncio.StreamReader(), FakeWriter())  # type: ignore[arg-type]
                conn.board_frame_base = bytes(lyrics_display_daemon.FRAME_BYTES)  # stale pre-swap frame
                daemon.boards.add(conn)
                await daemon.advance_schedule_after(
                    due_monotonic_ms=lyrics_display_daemon.monotonic_ms(),
                    generation=daemon.schedule_generation,
                )
                base_after_swap = conn.board_frame_base
                # Base invalidated -> the very next now-frame must be a full frame.
                short = bytearray(lyrics_display_daemon.FRAME_BYTES)
                short[0] = 1
                await daemon.send_frame(conn, bytes(short), now=True, swap_in_ms=0)
                frames = server_frame_payloads(b"".join(conn.writer.writes))
                envelopes = [parse_frame_envelope(payload) for opcode, payload in frames if opcode == 2]
                return base_after_swap, envelopes[-1]["kind"]

        base_after_swap, next_kind = asyncio.run(run())
        self.assertIsNone(base_after_swap)
        self.assertEqual(next_kind, lyrics_display_daemon.FRAME_KIND_FULL_NOW)


if __name__ == "__main__":
    unittest.main()

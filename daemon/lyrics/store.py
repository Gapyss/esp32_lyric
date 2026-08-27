"""Lyrics persistence (SQLite), LRC parsing and the lrclib client."""

from __future__ import annotations

import json
import re
import socket
import sqlite3
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from .constants import SQLITE_BUSY_TIMEOUT_SECONDS, LRCLIB_MAX_RETRY_AFTER_SECONDS, LRCLIB_RETRYABLE_STATUS, LRCLIB_RETRY_ATTEMPTS, LRCLIB_RETRY_DELAY_SECONDS, LRCLIB_TIMEOUT_SECONDS, LRCLIB_USER_AGENT, NEGATIVE_CACHE_SECONDS
from .state import Lyrics, TrackInfo

class LyricsStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        # The ESP32 and ESP8266 daemons are separate processes sharing this
        # cache file, and both write it (each resolves lyrics for its own
        # boards). The rollback journal's default whole-file write lock makes
        # that "database is locked"; WAL lets a writer and readers overlap, and
        # busy_timeout absorbs the writer-vs-writer case instead of raising.
        self.conn = sqlite3.connect(str(self.path), check_same_thread=False,
                                    timeout=SQLITE_BUSY_TIMEOUT_SECONDS)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute(f"PRAGMA busy_timeout={int(SQLITE_BUSY_TIMEOUT_SECONDS * 1000)}")
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

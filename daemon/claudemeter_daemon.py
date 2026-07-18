#!/usr/bin/env python3
"""Clawdmeter daemon (macOS).

Pushes YouTube Music now-playing state to the ESP32 render wrapper over HTTP.

The upstream ESP8266 daemon also pushes Claude usage percentages to /usage.
This ESP32 firmware currently only accepts /nowplaying, so usage polling is off
by default. Set CLAWDMETER_USAGE_SOURCE=api or local only when the target device
implements the compatible /usage endpoint.

Runs on system Python 3. YouTube Music duration metadata uses optional yt-dlp.
"""
import difflib
import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
from pathlib import Path

# ---- Edit if needed ----
DEVICE_URL = os.environ.get("CLAWDMETER_DEVICE_URL", "http://127.0.0.1:8123")
POLL_INTERVAL = 60                       # seconds between Claude usage polls
NOWPLAYING_TICK = 4                      # seconds between YouTube Music tab reads
# Lower = snappier track-skip / pause detection (the most visible "seam"). The read is
# a ~300ms Mac-side osascript+Chrome scriptlet, gated so it adds no extra DEVICE pushes
# (yt-dlp/lrclib only hit the network on identity change); 4s halves skip/pause lag vs 8s
# at ~7.5% read duty. The expensive ESP8266 thermal/WiFi constraints are device-side and
# unaffected by this. Don't go below the osascript round-trip time (~0.3s).
NOWPLAYING_RESYNC = 10                   # coarse position resync; avoid pushes every tick
NOWPLAYING_IDLE_TICK = int(os.environ.get("CLAWDMETER_NOWPLAYING_IDLE_TICK", "20"))
# When no song tab is open the 4s read just spawns osascript to get back "". Slow the
# tick while idle to cut those wasted spawns; snap back to NOWPLAYING_TICK the moment a
# song is playing (skip/pause detection stays at 4s — see NOWPLAYING_TICK). Cost: first
# detection after an idle stretch can lag up to this interval, which is fine when idle.
RAW_USAGE_SOURCE = os.environ.get("CLAWDMETER_USAGE_SOURCE", "off").lower()
USAGE_SOURCE = {"server": "api", "headers": "api"}.get(RAW_USAGE_SOURCE, RAW_USAGE_SOURCE)
USAGE_ENABLED = USAGE_SOURCE not in ("off", "none", "disabled", "0", "false")
RATE_LIMIT_BACKOFF = 10 * 60             # seconds when API does not send Retry-After
DEVICE_TIMEOUT = float(os.environ.get("CLAWDMETER_DEVICE_TIMEOUT", "5"))
DEVICE_PUSH_ATTEMPTS = int(os.environ.get("CLAWDMETER_DEVICE_PUSH_ATTEMPTS", "3"))
YTDLP_CACHE_SECONDS = int(os.environ.get("CLAWDMETER_YTDLP_CACHE_SECONDS", "900"))

# Local-log mode cannot know Anthropic's real server-side rate limit. Set these
# to the token budgets you want the ESP progress bars to represent.
SESSION_TOKEN_LIMIT = int(os.environ.get("CLAWDMETER_SESSION_TOKEN_LIMIT", "30000000"))
WEEKLY_TOKEN_LIMIT = int(os.environ.get("CLAWDMETER_WEEKLY_TOKEN_LIMIT", "100000000"))

# Daily-image metadata (xkcd + NASA APOD) for the ESP8266 COMIC/APOD screens.
# The ESP8266 cannot afford BearSSL's ~22 KB TLS heap to call these HTTPS APIs
# itself, so this daemon resolves them and pushes image URL + title to /daily.
# "auto" = only when usage pushing is on (i.e. the target is the ESP8266; the
# ESP32 render wrapper has no /daily endpoint).
_RAW_DAILY = os.environ.get("CLAWDMETER_DAILY_IMAGES", "auto").lower()
NASA_API_KEY = os.environ.get("CLAWDMETER_NASA_API_KEY", "DEMO_KEY")
DAILY_INTERVAL = int(os.environ.get("CLAWDMETER_DAILY_INTERVAL", str(6 * 3600)))
DAILY_RETRY = 10 * 60                    # retry sooner after a fetch/push failure
DAILY_ENABLED = (USAGE_ENABLED if _RAW_DAILY == "auto"
                 else _RAW_DAILY not in ("off", "none", "disabled", "0", "false"))
# ------------------------

KEYCHAIN_SERVICE = "Claude Code-credentials"
API_URL = "https://api.anthropic.com/v1/messages"
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

PROJECT_DIRS = (
    Path.home() / ".claude" / "projects",
    Path.home() / "Library" / "Developer" / "Xcode" /
    "CodingAssistant" / "ClaudeAgentConfig" / "projects",
)


def parse_timestamp(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)
    except ValueError:
        return None


def turn_tokens(record):
    """Return (timestamp, token_count, message_id) for assistant usage records."""
    if record.get("type") != "assistant":
        return None

    msg = record.get("message") or {}
    usage = msg.get("usage") or {}
    tokens = (
        (usage.get("input_tokens") or 0) +
        (usage.get("output_tokens") or 0) +
        (usage.get("cache_read_input_tokens") or 0) +
        (usage.get("cache_creation_input_tokens") or 0)
    )
    if tokens <= 0:
        return None

    ts = parse_timestamp(record.get("timestamp"))
    if ts is None:
        return None

    return ts, tokens, msg.get("id") or ""


def get_token():
    """Read the Claude Code OAuth access token from the macOS Keychain."""
    out = subprocess.check_output(
        ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
        text=True,
    ).strip()
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return out
    stack = [data]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            tok = node.get("accessToken")
            if isinstance(tok, str):
                return tok
            stack.extend(node.values())
        elif isinstance(node, list):
            stack.extend(node)
    raise RuntimeError("accessToken not found in Keychain item")


def pct(value):
    """Header utilization is a 0..1 fraction; convert to an integer percent."""
    try:
        return round(float(value) * 100)
    except (TypeError, ValueError):
        return -1


def usage_from_headers(headers):
    return (
        pct(headers.get("anthropic-ratelimit-unified-5h-utilization")),
        pct(headers.get("anthropic-ratelimit-unified-7d-utilization")),
    )


def int_or_zero(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


# representative-claim tells which limit is the binding one; the device draws an
# amber stripe on that block. 1 = session (5h), 2 = weekly (7d), 0 = unknown.
_BIND = {"five_hour": 1, "seven_day": 2}


def extra_from_headers(headers):
    """Reset epochs, allow/deny status, binding limit, and server clock.

    These ride along with the two utilization headers and let the device show
    real reset times + a UTC clock without an RTC or NTP.
    """
    server_epoch = 0
    date_hdr = headers.get("Date")
    if date_hdr:
        try:
            server_epoch = int(parsedate_to_datetime(date_hdr).timestamp())
        except (TypeError, ValueError):
            server_epoch = 0
    return {
        "sr": int_or_zero(headers.get("anthropic-ratelimit-unified-5h-reset")),
        "wr": int_or_zero(headers.get("anthropic-ratelimit-unified-7d-reset")),
        "stat": (headers.get("anthropic-ratelimit-unified-status") or "").strip(),
        "bind": _BIND.get(
            (headers.get("anthropic-ratelimit-unified-representative-claim") or "").strip(), 0),
        "t": server_epoch,
    }


def retry_after_seconds(headers):
    try:
        return max(1, int(headers.get("retry-after", "")))
    except ValueError:
        return RATE_LIMIT_BACKOFF


def run_text(cmd, timeout=5):
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL, timeout=timeout)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


def bounded_pct(value):
    try:
        return max(0, min(100, round(float(value))))
    except (TypeError, ValueError):
        return -1


def cpu_percent():
    out = run_text(["ps", "-A", "-o", "%cpu="])
    total = 0.0
    for line in out.splitlines():
        try:
            total += float(line.strip())
        except ValueError:
            pass
    cores = os.cpu_count() or 1
    return bounded_pct(total / cores)


def memory_percent():
    vm = run_text(["vm_stat"])

    pages = {}
    for line in vm.splitlines():
        m = re.match(r"Pages ([^:]+):\s+([0-9.]+)", line)
        if m:
            pages[m.group(1)] = int(m.group(2).replace(".", ""))

    free = pages.get("free", 0) + pages.get("speculative", 0)
    used = (
        pages.get("active", 0) +
        pages.get("wired down", 0) +
        pages.get("occupied by compressor", 0)
    )
    total = free + used + pages.get("inactive", 0)
    if total <= 0:
        return -1
    return bounded_pct(used * 100 / total)


def disk_percent():
    try:
        usage = shutil.disk_usage(str(Path.home()))
    except OSError:
        return -1
    return bounded_pct(usage.used * 100 / usage.total)


def battery_percent():
    out = run_text(["pmset", "-g", "batt"])
    m = re.search(r"(\d+)%", out)
    return bounded_pct(m.group(1)) if m else -1


def mac_metrics():
    return {
        "cpu": cpu_percent(),
        "mem": memory_percent(),
        "disk": disk_percent(),
        "bat": battery_percent(),
    }


# ---- YouTube Music now-playing (read from a Chrome tab) ----
# We read the tab title, URL, and (via page JS) the live MediaSession metadata of
# any open YouTube Music tab; MediaSession title/artist is preferred over the tab
# title, which often stays "YouTube Music" for home-feed playback. The guard at the
# top returns "" before the "Google Chrome" tell block runs when Chrome is not
# already running, so a background daemon never launches the browser. Current
# position/duration require Chrome's View > Developer > Allow JavaScript from Apple
# Events.
#
# Position/duration are read from YT Music's own player UI (`#progress-bar`'s
# aria-valuenow/aria-valuemax), NOT from the `<video>` element. During autoplay /
# radio-mix advance YT Music streams the whole queue through one continuous MSE
# `<video>`, so `video.currentTime`/`video.duration` are the *cumulative* timeline
# of every track played so far (they keep growing and never reset on song change) —
# the 2nd autoplay song would report a multi-hundred-second "duration". The
# progress bar resets per song and its aria-valuemax is the real track length.
NOWPLAYING_SUFFIXES = (" - YouTube Music", " | YouTube Music")
YTDLP_CACHE = {}
YTDLP_WARNED = False
LRCLIB_CACHE = {}
LRCLIB_FAIL = {}          # key -> monotonic time of last network failure (negative cache)
LRCLIB_WARNED = False
LRCLIB_TIMEOUT = float(os.environ.get("CLAWDMETER_LRCLIB_TIMEOUT", "5"))
# After a network failure, don't hammer lrclib every tick: back off per-song so a
# sustained outage costs one (timeout-bounded) attempt every few minutes, not one
# every NOWPLAYING_TICK seconds.
LRCLIB_RETRY_BACKOFF = float(os.environ.get("CLAWDMETER_LRCLIB_RETRY_BACKOFF", "300"))
LYRIC_ADVANCE_SECONDS = float(os.environ.get("CLAWDMETER_LYRIC_ADVANCE_SECONDS", "4.5"))
LYRIC_SYNC_OFFSET = float(os.environ.get("CLAWDMETER_LYRIC_SYNC_OFFSET", "1.0"))
MAX_LYRIC_CHARS = int(os.environ.get("CLAWDMETER_MAX_LYRIC_CHARS", "64"))
LRCLIB_USER_AGENT = os.environ.get(
    "CLAWDMETER_LRCLIB_USER_AGENT",
    "Clawdmeter/1.0 (https://github.com/HermannBjorgvin/Clawdmeter)",
)
# Optional: path to a downloaded lrclib SQLite dump (https://lrclib.net/db-dumps).
# When set and valid, lyric lookups are served locally (offline, sub-ms, no rate
# limits) instead of hitting lrclib.net. A clean miss stays local (offline intent);
# only a sqlite *error* falls back to the network path.
LRCLIB_DB_PATH = os.environ.get("CLAWDMETER_LRCLIB_DB", "").strip()
_LRCLIB_DB = None             # cached read-only sqlite3.Connection
_LRCLIB_DB_DISABLED = False   # set once if the dump is missing/unusable
# Columns the lrclib dump's `tracks` table must have for our queries to work.
_LRCLIB_TRACK_COLS = {
    "name", "name_lower", "artist_name", "artist_name_lower",
    "duration", "last_lyrics_id",
}
# Persistent on-demand lyric cache: every song fetched from lrclib.net is saved
# here, so replays are offline/instant and the in-memory cache survives restarts.
# Grows only with songs actually played (a few MB), unlike the full ~80 GB dump.
# Set the env var to empty to disable.
LYRIC_CACHE_PATH = os.environ.get(
    "CLAWDMETER_LYRIC_CACHE", str(Path.home() / ".clawdmeter" / "lyrics.sqlite3")
).strip()
_LYRIC_CACHE_DB = None
_LYRIC_CACHE_DISABLED = False
NOWPLAYING_SCRIPT = (
    'tell application "System Events"\n'
    '  if not ((name of processes) contains "Google Chrome") then return ""\n'
    'end tell\n'
    'tell application "Google Chrome"\n'
    '  repeat with w in windows\n'
    '    repeat with t in tabs of w\n'
    '      set ti to title of t\n'
    '      set u to URL of t\n'
    '      if u contains "music.youtube.com" or ti ends with "- YouTube Music" or ti ends with "| YouTube Music" then\n'
    '        set meta to "-1|-1|-1"\n'
    '        try\n'
    '          set meta to execute t javascript "(function(){var v=document.querySelector(\'video\');var p=v?(v.paused?1:0):-1;var b=document.querySelector(\'#progress-bar\');var n=b?parseFloat(b.getAttribute(\'aria-valuenow\')):NaN;var m=b?parseFloat(b.getAttribute(\'aria-valuemax\')):NaN;var pos=isFinite(n)?Math.floor(n):-1;var dur=(isFinite(m)&&m>0)?Math.floor(m):-1;var md=(navigator.mediaSession&&navigator.mediaSession.metadata)||{};var mt=md.title||\'\';var ma=md.artist||\'\';return pos+\'|\'+dur+\'|\'+p+\'\\t\'+mt+\'\\t\'+ma;})()"\n'
    '        end try\n'
    '        return ti & linefeed & u & linefeed & meta\n'
    '      end if\n'
    '    end repeat\n'
    '  end repeat\n'
    'end tell\n'
    'return ""\n'
)


def parse_nowplaying_title(raw):
    for suffix in NOWPLAYING_SUFFIXES:
        if raw.endswith(suffix):
            raw = raw[:-len(suffix)]
            break
    raw = raw.strip()
    if " - " in raw:
        title, artist = raw.rsplit(" - ", 1)
        return (title.strip(), artist.strip())
    return (raw, "")


def parse_playback_meta(raw):
    try:
        pos, dur, paused = raw.split("|", 2)
        paused_i = int(float(paused))
        if paused_i not in (-1, 0, 1):
            paused_i = -1
        return int(float(pos)), int(float(dur)), paused_i
    except (AttributeError, TypeError, ValueError):
        return -1, -1, -1


def ytdlp_metadata(url, title="", artist=""):
    """Return static metadata from yt-dlp, or {} if yt-dlp is unavailable/fails."""
    global YTDLP_WARNED
    if not url and not title:
        return {}

    now = time.monotonic()
    source = url if ("watch?" in url or "youtu.be/" in url) else ""
    is_search = False
    if not source and title:
        source = "ytsearch1:" + " ".join(part for part in (title, artist) if part).strip()
        is_search = True
    if not source:
        return {}

    cached = YTDLP_CACHE.get(source)
    if cached and now - cached[0] < YTDLP_CACHE_SECONDS:
        return cached[1]

    try:
        import yt_dlp
    except ImportError:
        if not YTDLP_WARNED:
            print("yt-dlp metadata disabled: install with `python3 -m pip install yt-dlp`",
                  file=sys.stderr)
            YTDLP_WARNED = True
        return {}

    opts = {
        "quiet": True,
        "no_warnings": True,
        "skip_download": True,
        "extract_flat": False,
        "noplaylist": True,
        "socket_timeout": 10,  # don't let a slow YouTube response stall the now-playing tick
    }
    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(source, download=False)
    except Exception as e:
        print(f"yt-dlp metadata failed: {e}", file=sys.stderr)
        return {}

    if info.get("_type") == "playlist" and info.get("entries"):
        info = info["entries"][0] or {}

    meta = {
        "title": (info.get("track") or info.get("title") or "").strip(),
        "artist": (info.get("artist") or info.get("uploader") or "").strip(),
        "duration": int(info.get("duration") or -1),
        "search": is_search,
    }
    YTDLP_CACHE[source] = (now, meta)
    return meta


def read_now_playing():
    """Return (title, artist, pos, dur, paused) for YouTube Music, else empty.

    YT Music's tab title varies. It may be "Song - Artist - YouTube Music",
    or just "Song | YouTube Music", so parse defensively rather than with a
    strict pattern. yt-dlp augments the title-derived metadata with canonical
    duration; browser JS supplies current position when Chrome allows it.
    """
    raw = run_text(["osascript", "-e", NOWPLAYING_SCRIPT], timeout=4).strip()
    if not raw:
        return ("", "", -1, -1, -1)

    parts = raw.splitlines()
    title, artist = parse_nowplaying_title(parts[0] if parts else "")
    url = parts[1].strip() if len(parts) > 1 else ""
    # The meta line is "pos|dur|paused\tmediaTitle\tmediaArtist". A tab separator is
    # used (not a newline) because AppleScript's double-quoted string literal eats
    # \n/\r/\t escapes and turns them into real control chars in the JS source — a
    # real newline there makes Chrome's `execute javascript` bridge yield "missing
    # value". \t survives as a literal tab (valid inside a JS string), so it works;
    # for the same reason the JS must not contain a /[\r\n\t]/ regex.
    meta_fields = (parts[2] if len(parts) > 2 else "").split("\t")
    pos, browser_dur, paused = parse_playback_meta(meta_fields[0])
    # The tab title can lag or stay plain "YouTube Music" when a song is played from
    # the home feed, so prefer the page's MediaSession metadata when present. Falls
    # back to the tab title when JS is blocked (Allow JavaScript from Apple Events
    # disabled) — same graceful degradation as pos/dur.
    media_title = meta_fields[1].strip() if len(meta_fields) > 1 else ""
    media_artist = meta_fields[2].strip() if len(meta_fields) > 2 else ""
    if media_title:
        title = media_title
        artist = media_artist or artist
    meta = ytdlp_metadata(url, title, artist)

    if meta.get("title") and not meta.get("search"):
        title = meta["title"]
    if meta.get("artist") and not artist and not meta.get("search"):
        artist = meta["artist"]
    # YT Music's player progress bar (read in NOWPLAYING_SCRIPT) is authoritative for
    # the currently loaded track's duration. yt-dlp is only a fallback: URL/search
    # metadata can lag or resolve the wrong version during YouTube Music auto-advance.
    # (The raw <video> element is deliberately NOT used for duration — see the
    # NOWPLAYING_SCRIPT note: it reports the cumulative autoplay-queue timeline.)
    dur = browser_dur if browser_dur > 0 else (meta.get("duration") or -1)
    return (title, artist, pos, dur, paused)


def normalize_nowplaying_timing(song, last_song):
    """Drop transient Chrome timing that cannot belong to the reported song."""
    title, artist, pos, dur, paused = song
    last_title, last_artist, _last_pos, _last_dur, _last_paused = last_song
    track_changed = bool(last_title or last_artist) and (
        title != last_title or artist != last_artist
    )

    if not title or pos < 0 or dur <= 0:
        return song
    if track_changed:
        if pos >= max(dur - 2, 0):
            dur = -1
        pos = 0
    elif pos > dur:
        pos = dur
    return (title, artist, pos, dur, paused)


def lyric_key(title, artist, dur):
    if not title:
        return None
    dur_key = int(dur) if dur and dur > 0 else 0
    return (
        re.sub(r"\s+", " ", title).strip().lower(),
        re.sub(r"\s+", " ", artist or "").strip().lower(),
        dur_key,
    )


def clean_lyric_line(line):
    return re.sub(r"\s+", " ", (line or "").strip())


def parse_lrc(text):
    """Parse LRC synced lyrics into [(seconds, line), ...]."""
    lines = []
    for raw in (text or "").splitlines():
        stamps = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", raw)
        if not stamps:
            continue
        lyric = clean_lyric_line(re.sub(r"(?:\[\d+:\d+(?:\.\d+)?\])+", "", raw))
        if not lyric:
            continue
        for minutes, seconds in stamps:
            try:
                lines.append((int(minutes) * 60 + float(seconds), lyric))
            except ValueError:
                pass
    lines.sort(key=lambda item: item[0])
    return lines


def parse_plain_lyrics(text):
    return [clean_lyric_line(line) for line in (text or "").splitlines()
            if clean_lyric_line(line)]


def empty_lyrics():
    return {"synced": [], "plain": [], "instrumental": False}


def lyrics_from_lrclib_payload(payload):
    if not isinstance(payload, dict):
        return empty_lyrics()
    if payload.get("instrumental"):
        return {"synced": [], "plain": [], "instrumental": True}
    synced = parse_lrc(payload.get("syncedLyrics") or "")
    plain = parse_plain_lyrics(payload.get("plainLyrics") or "")
    return {"synced": synced, "plain": plain, "instrumental": False}


def lrclib_db():
    """Return a cached read-only connection to the local lrclib dump, or None.

    Returns None (and warns once) when no dump is configured, the file is
    missing, or the schema doesn't look like an lrclib dump — callers then fall
    back to the HTTP API. Opened with mode=ro&immutable=1: the dump is a static
    snapshot, so this skips lock files / -wal handling and works on read-only
    media.
    """
    global _LRCLIB_DB, _LRCLIB_DB_DISABLED
    if _LRCLIB_DB_DISABLED or not LRCLIB_DB_PATH:
        return None
    if _LRCLIB_DB is not None:
        return _LRCLIB_DB
    try:
        if not os.path.exists(LRCLIB_DB_PATH):
            raise FileNotFoundError(LRCLIB_DB_PATH)
        uri = f"file:{urllib.request.pathname2url(LRCLIB_DB_PATH)}?mode=ro&immutable=1"
        conn = sqlite3.connect(uri, uri=True, check_same_thread=False)
        cols = {row[1] for row in conn.execute("PRAGMA table_info(tracks)")}
        if not _LRCLIB_TRACK_COLS.issubset(cols):
            raise sqlite3.DatabaseError(f"unexpected tracks schema: {sorted(cols)}")
        conn.execute("PRAGMA mmap_size=30000000000")  # memory-map; no row copies
        conn.row_factory = sqlite3.Row
        _LRCLIB_DB = conn
        print(f"lrclib: serving lyrics from local dump {LRCLIB_DB_PATH}")
        return _LRCLIB_DB
    except (OSError, sqlite3.Error) as e:
        _LRCLIB_DB_DISABLED = True
        print(f"lrclib: local dump unavailable ({e}); using network", file=sys.stderr)
        return None


def _lrclib_row_payload(row):
    """Shape a (tracks JOIN lyrics) row like an lrclib.net API JSON object."""
    return {
        "trackName": row["name"],
        "artistName": row["artist_name"],
        "duration": row["duration"],
        "syncedLyrics": row["synced_lyrics"],
        "plainLyrics": row["plain_lyrics"],
        "instrumental": bool(row["instrumental"]),
    }


_LRCLIB_SELECT = (
    "SELECT t.name, t.artist_name, t.duration, "
    "l.synced_lyrics, l.plain_lyrics, l.instrumental "
)
# Word runs, Unicode-aware. Thai has no word spaces so a Thai title is one token;
# fuzzy FTS search on it is weak (unicode61 doesn't segment Thai) — but the exact
# name_lower= path below carries Thai fine, and that's the common case anyway.
_FTS_TOKEN_RE = re.compile(r"\w+", re.UNICODE)


def lrclib_db_get(conn, params):
    """Local equivalent of GET /api/get: exact title (+artist, +/-2s duration)."""
    name = (params.get("track_name") or "").strip().lower()
    if not name:
        return None
    sql = _LRCLIB_SELECT + "FROM tracks t JOIN lyrics l ON l.id = t.last_lyrics_id WHERE t.name_lower = ?"
    args = [name]
    artist = (params.get("artist_name") or "").strip().lower()
    if artist:
        sql += " AND t.artist_name_lower = ?"
        args.append(artist)
    dur = params.get("duration")
    if dur not in (None, "", -1):
        # lrclib's get tolerates ~2s; pick the closest within tolerance.
        sql += " AND ABS(t.duration - ?) <= 2 ORDER BY ABS(t.duration - ?) LIMIT 1"
        args += [float(dur), float(dur)]
    else:
        sql += " LIMIT 1"
    row = conn.execute(sql, args).fetchone()
    return _lrclib_row_payload(row) if row else None


def lrclib_db_search(conn, params):
    """Local equivalent of GET /api/search: FTS5 over title+artist, bm25-ranked."""
    terms = f"{params.get('track_name') or ''} {params.get('artist_name') or ''}"
    tokens = _FTS_TOKEN_RE.findall(terms.lower())
    if not tokens:
        return []
    match = " OR ".join('"' + t.replace('"', "") + '"' for t in tokens)
    sql = (
        _LRCLIB_SELECT
        + "FROM tracks_fts f JOIN tracks t ON t.id = f.rowid "
        "JOIN lyrics l ON l.id = t.last_lyrics_id "
        "WHERE tracks_fts MATCH ? ORDER BY rank LIMIT 20"
    )
    return [_lrclib_row_payload(r) for r in conn.execute(sql, (match,)).fetchall()]


def _lrclib_http(endpoint, params):
    query = urllib.parse.urlencode(
        {k: v for k, v in params.items() if v not in ("", None, -1)}
    )
    req = urllib.request.Request(
        f"https://lrclib.net{endpoint}?{query}",
        headers={
            "Accept": "application/json",
            "User-Agent": LRCLIB_USER_AGENT,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=LRCLIB_TIMEOUT) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def lrclib_json(endpoint, params):
    """Dispatch a lrclib lookup to the local dump if configured, else the API.

    A clean local miss returns None/[] and stays offline; a sqlite *error* falls
    through to the network so a corrupt/locked dump never breaks lyrics.
    """
    conn = lrclib_db()
    if conn is not None:
        try:
            if endpoint == "/api/get":
                return lrclib_db_get(conn, params)
            if endpoint == "/api/search":
                return lrclib_db_search(conn, params)
        except sqlite3.Error as e:
            print(f"lrclib: local query failed ({e}); using network", file=sys.stderr)
    return _lrclib_http(endpoint, params)


# Strip "(feat. X)" / "[ft …]" / "(ร่วมกับ X)" / "(prod. …)" tails before fuzzy
# compare, so a query that keeps the suffix still matches a candidate without it.
_FEAT_RE = re.compile(
    r"\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with|prod\.?|ร่วมกับ)\b[^)\]]*[\)\]]?",
    re.IGNORECASE,
)
_MATCH_PUNCT_RE = re.compile(r"[^\w\s]", re.UNICODE)


def _norm_match_text(s):
    """Normalize a title/artist for fuzzy comparison: lowercase, drop feat./ร่วมกับ
    parentheticals and punctuation, collapse whitespace. Thai is left intact — no
    unicode decomposition, which would split combining tone/vowel marks."""
    s = (s or "").strip().lower()
    s = _FEAT_RE.sub(" ", s)
    s = _MATCH_PUNCT_RE.sub(" ", s)
    return re.sub(r"\s+", " ", s).strip()


# Cosmetic title suffixes that mark the SAME recording but vary between YT Music
# contexts and lrclib: remasters and version tags. Stripped only for tolerant
# *matching* — never for cache keys. (Live)/(Acoustic)/(Demo) are deliberately
# NOT here: those are different recordings with different lyrics/timing.
_TITLE_NOISE_RE = re.compile(
    r"\s*[\(\[]\s*[^)\]]*\b(?:remaster(?:ed)?|original\s+version|mono|stereo|"
    r"album\s+version|single\s+version)\b[^)\]]*[\)\]]"
    r"|\s*-\s*(?:\d{2,4}\s+)?(?:remaster(?:ed)?|original\s+version|mono|stereo)\b.*$",
    re.IGNORECASE,
)


def _canonical_title(s):
    """Lowercased title with feat./remaster/version noise removed, for tolerant
    matching across YT Music's cosmetic title variants. NOT used for cache keys."""
    s = (s or "").strip().lower()
    s = _FEAT_RE.sub(" ", s)
    s = _TITLE_NOISE_RE.sub(" ", s)
    s = _MATCH_PUNCT_RE.sub(" ", s)
    return re.sub(r"\s+", " ", s).strip()


def _fuzzy_ratio(a, b):
    return difflib.SequenceMatcher(None, a, b).ratio() if a and b else 0.0


def lrclib_score(candidate, title, artist, dur):
    score = 0
    cand_title = (candidate.get("trackName") or "").strip().lower()
    cand_artist = (candidate.get("artistName") or "").strip().lower()
    want_title = (title or "").strip().lower()
    want_artist = (artist or "").strip().lower()
    if cand_title == want_title:
        score += 50
    elif want_title and want_title in cand_title:
        score += 20
    elif want_title:
        # Slightly-wrong title (typo, dropped feat. suffix, punctuation): grade by
        # fuzzy similarity of the normalized strings so a near-match still ranks.
        # Strictly additive — exact/substring hits above are untouched.
        ratio = _fuzzy_ratio(_norm_match_text(title), _norm_match_text(cand_title))
        if ratio >= 0.6:
            score += int(40 * ratio)
    if want_artist and cand_artist == want_artist:
        score += 30
    elif want_artist and want_artist in cand_artist:
        score += 10
    elif want_artist:
        ratio = _fuzzy_ratio(_norm_match_text(artist), _norm_match_text(cand_artist))
        if ratio >= 0.6:
            score += int(20 * ratio)
    cand_dur = int(candidate.get("duration") or 0)
    if dur and dur > 0 and cand_dur > 0:
        delta = abs(cand_dur - int(dur))
        if delta <= 2:
            score += 20
        elif delta <= 8:
            score += 8
        else:
            score -= min(delta, 30)
    if candidate.get("syncedLyrics"):
        score += 5
    if candidate.get("plainLyrics"):
        score += 2
    return score


def lyric_cache_db():
    """Lazily open (creating if needed) the persistent on-demand lyric cache.

    Returns a sqlite3 connection, or None if disabled / unwritable (warn once,
    then callers just skip the cache and use the network as before).
    """
    global _LYRIC_CACHE_DB, _LYRIC_CACHE_DISABLED
    if _LYRIC_CACHE_DISABLED or not LYRIC_CACHE_PATH:
        return None
    if _LYRIC_CACHE_DB is not None:
        return _LYRIC_CACHE_DB
    try:
        Path(LYRIC_CACHE_PATH).parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(LYRIC_CACHE_PATH, check_same_thread=False)
        conn.execute(
            "CREATE TABLE IF NOT EXISTS lyrics_cache ("
            "key TEXT PRIMARY KEY, track TEXT, artist TEXT, duration REAL, "
            "synced_lyrics TEXT, plain_lyrics TEXT, instrumental INTEGER, updated REAL)"
        )
        conn.commit()
        _LYRIC_CACHE_DB = conn
        return _LYRIC_CACHE_DB
    except (OSError, sqlite3.Error) as e:
        _LYRIC_CACHE_DISABLED = True
        print(f"lyric cache disabled ({e}); using network only", file=sys.stderr)
        return None


def _lyric_cache_key(key):
    # lyric_key() is a (title, artist, dur) tuple; \x1f can't occur in the text.
    return "\x1f".join((key[0], key[1], str(key[2])))


def lyric_cache_get(key):
    conn = lyric_cache_db()
    if conn is None:
        return None
    try:
        row = conn.execute(
            "SELECT synced_lyrics, plain_lyrics, instrumental FROM lyrics_cache WHERE key = ?",
            (_lyric_cache_key(key),),
        ).fetchone()
    except sqlite3.Error:
        return None
    if row is None:
        return None
    return lyrics_from_lrclib_payload(
        {"syncedLyrics": row[0], "plainLyrics": row[1], "instrumental": bool(row[2])}
    )


def lyric_cache_put(key, payload):
    conn = lyric_cache_db()
    if conn is None or not isinstance(payload, dict):
        return
    try:
        conn.execute(
            "INSERT OR REPLACE INTO lyrics_cache "
            "(key, track, artist, duration, synced_lyrics, plain_lyrics, instrumental, updated) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (
                _lyric_cache_key(key),
                payload.get("trackName"), payload.get("artistName"), payload.get("duration"),
                payload.get("syncedLyrics"), payload.get("plainLyrics"),
                int(bool(payload.get("instrumental"))), time.time(),
            ),
        )
        conn.commit()
    except sqlite3.Error as e:
        print(f"lyric cache write failed ({e})", file=sys.stderr)


def tolerant_cache_lookup(title, dur):
    """Artist-blind, title-canonical, duration±2s scan of the persistent cache.

    Rescues the SAME recording when YT Music reports it with a cosmetic title
    suffix (e.g. "(Remastered 2015)") or a different-script artist than the cached
    copy (e.g. Thai "บอย โกสิยพงษ์" vs romanized "Boyd Kosiyabong"). The duration
    gate is a HARD ±2s so two same-title covers (Beatles vs John Denver "Let It Be")
    can never collide. Returns (parsed, raw_payload) or None — a full-table scan over
    the tiny cache is microseconds.
    """
    conn = lyric_cache_db()
    if conn is None or not dur or dur <= 0:
        return None
    want = _canonical_title(title)
    if not want:
        return None
    try:
        rows = conn.execute(
            "SELECT key, synced_lyrics, plain_lyrics, instrumental FROM lyrics_cache"
        ).fetchall()
    except sqlite3.Error:
        return None
    best = None
    best_delta = 3  # only |delta| <= 2 qualifies
    for key, synced, plain, instrumental in rows:
        parts = key.split("\x1f")
        if len(parts) != 3:
            continue
        cand_title, _cand_artist, cand_dur = parts
        try:
            cand_dur = int(cand_dur)
        except ValueError:
            continue
        if _canonical_title(cand_title) != want:
            continue
        delta = abs(cand_dur - int(dur))
        if delta < best_delta:
            best_delta = delta
            best = (synced, plain, instrumental)
            if delta == 0:
                break
    if best is None:
        return None
    payload = {"syncedLyrics": best[0] or "", "plainLyrics": best[1] or "",
               "instrumental": bool(best[2])}
    return lyrics_from_lrclib_payload(payload), payload


def fetch_lyrics(title, artist, dur):
    """Return cached parsed lyrics for the current song, fetching from lrclib if needed."""
    global LRCLIB_WARNED
    key = lyric_key(title, artist, dur)
    if key is None:
        return empty_lyrics()
    if key in LRCLIB_CACHE:
        return LRCLIB_CACHE[key]
    cached = lyric_cache_get(key)
    if cached is not None:
        LRCLIB_CACHE[key] = cached
        return cached
    # Tolerant local rescue: the same recording may already be cached under a
    # cosmetic title variant or a different-script artist. ±2s-gated, so safe to
    # write through under the live key — next lookup is then an exact O(1) hit.
    local = tolerant_cache_lookup(title, dur)
    if local is not None:
        parsed, payload = local
        LRCLIB_CACHE[key] = parsed
        lyric_cache_put(key, {**payload, "trackName": title,
                              "artistName": artist, "duration": dur})
        return parsed
    fail_at = LRCLIB_FAIL.get(key)
    if fail_at is not None and time.monotonic() - fail_at < LRCLIB_RETRY_BACKOFF:
        return empty_lyrics()

    params = {"track_name": title, "artist_name": artist}
    if dur and dur > 0:
        params["duration"] = int(dur)

    payload = None
    try:
        payload = lrclib_json("/api/get", params)
        if payload is None:
            results = lrclib_json("/api/search", {
                "track_name": title,
                "artist_name": artist,
            })
            if results:
                payload = max(results, key=lambda item: lrclib_score(item, title, artist, dur))
        if payload is None and dur and dur > 0:
            # Artist-blind last resort: YT Music's artist (e.g. Thai script) may not
            # match lrclib's romanized artist at all, so search by title only — then
            # require a HARD ±2s duration match so a same-title cover can't slip in.
            results = lrclib_json("/api/search", {"track_name": title})
            if results:
                cand = max(results, key=lambda item: lrclib_score(item, title, "", dur))
                cand_dur = int(cand.get("duration") or 0)
                if cand_dur and abs(cand_dur - int(dur)) <= 2:
                    payload = cand
    except (TimeoutError, socket.timeout, urllib.error.URLError, json.JSONDecodeError) as e:
        LRCLIB_FAIL[key] = time.monotonic()
        if not LRCLIB_WARNED:
            print(f"lrclib lookup failed; backing off {int(LRCLIB_RETRY_BACKOFF)}s: {e}",
                  file=sys.stderr)
            LRCLIB_WARNED = True
        return empty_lyrics()

    LRCLIB_FAIL.pop(key, None)
    parsed = lyrics_from_lrclib_payload(payload)
    LRCLIB_CACHE[key] = parsed
    # Persist real results only — never cache "none found", so a song missing
    # today can still be picked up later (negative results stay session-only).
    if parsed["instrumental"] or parsed["synced"] or parsed["plain"]:
        lyric_cache_put(key, payload)
    if parsed["instrumental"]:
        kind = "instrumental"
    elif parsed["synced"]:
        kind = f"synced ({len(parsed['synced'])} lines)"
    elif parsed["plain"]:
        kind = f"plain ({len(parsed['plain'])} lines)"
    else:
        kind = "none found"
    print(f"lyrics: {kind} for {title}" + (f" — {artist}" if artist else ""))
    return parsed


def synced_lyric_payload(lines, pos):
    idx = -1
    for i, (line_pos, _line) in enumerate(lines):
        if line_pos <= pos:
            idx = i
        else:
            break
    current = lines[idx][1] if idx >= 0 else ""
    next_idx = idx + 1
    next_line = lines[next_idx][1] if next_idx < len(lines) else ""
    third_idx = idx + 2
    third_line = lines[third_idx][1] if third_idx < len(lines) else ""
    # Firmware stores lt as integer seconds. Round up so a fractional LRC stamp
    # never promotes the next line early.
    next_at = int(lines[next_idx][0] + 0.999) if next_idx < len(lines) else -1
    third_at = int(lines[third_idx][0] + 0.999) if third_idx < len(lines) else -1
    return current, next_line, third_line, next_at, third_at, idx


def lyric_payload(title, artist, pos, dur, paused, now, state):
    """Return (lyric, lyric2, lyric3, lt, lt2) for the current tick."""
    key = lyric_key(title, artist, dur)
    # Track continuity uses (title, artist) only, not the full lyric_key. `dur` can
    # legitimately flip by a second between polls (browser_dur missing on a given
    # read falls back to yt-dlp's duration, which need not match exactly — see
    # read_now_playing). Resetting state on that flicker was fine for synced lyrics
    # (recomputed fresh from `pos` every call) but silently froze plain/unsynced
    # lyrics: plain_index and plain_last got wiped every tick, so elapsed time
    # since plain_last never accumulated enough to advance past line 0.
    track_key = (key[0], key[1]) if key is not None else None
    if track_key != state.get("track_key"):
        state.clear()
        state.update({"track_key": track_key, "plain_index": 0, "plain_last": now, "synced_index": None})

    if key is None:
        return "", "", "", -1, -1

    parsed = fetch_lyrics(title, artist, dur)
    synced = parsed.get("synced") or []
    if synced and pos >= 0:
        lyric, lyric2, lyric3, next_at, third_at, idx = synced_lyric_payload(
            synced, pos + LYRIC_SYNC_OFFSET
        )
        state["synced_index"] = idx
        return lyric, lyric2, lyric3, next_at, third_at

    plain = parsed.get("plain") or []
    if not plain:
        return "", "", "", -1, -1

    # Plain (unsynced) lyrics have no timestamps, so we advance them on a wall-clock
    # cadence — but only while playing. While paused, hold the line and keep the timer
    # base at `now` so resume doesn't jump (and lyric_changed stays False, so a paused
    # track stops emitting a fresh push every tick). Cadence is wall-clock
    # (LYRIC_ADVANCE_SECONDS via `now - plain_last`), not per-tick, so it's independent of
    # NOWPLAYING_TICK — a faster tick just tracks the intended cadence more closely. This
    # only matters for lyrics that have nothing real to sync against.
    if paused == 1:
        state["plain_last"] = now
    else:
        elapsed = now - state.get("plain_last", now)
        if elapsed >= LYRIC_ADVANCE_SECONDS:
            steps = int(elapsed / LYRIC_ADVANCE_SECONDS)
            state["plain_index"] = min(len(plain) - 1, state.get("plain_index", 0) + steps)
            state["plain_last"] = now
    idx = state.get("plain_index", 0)
    return (
        plain[idx],
        plain[idx + 1] if idx + 1 < len(plain) else "",
        plain[idx + 2] if idx + 2 < len(plain) else "",
        -1,
        -1,
    )


def push_now_playing(title, artist, pos=-1, dur=-1, paused=-1,
                     lyric="", lyric2="", lyric3="", lt=-1, lt2=-1):
    """Best-effort push of the current song. UTF-8 is preserved (Thai stays Thai)."""
    quote = urllib.parse.quote
    # Defensive cap: the device clips lyric lines at the panel edge (no marquee), so a
    # pathologically long line gains nothing on-screen and only bloats the request URL.
    # Title/artist are intentionally NOT capped — they marquee-scroll on the device.
    lyric = lyric[:MAX_LYRIC_CHARS]
    lyric2 = lyric2[:MAX_LYRIC_CHARS]
    lyric3 = lyric3[:MAX_LYRIC_CHARS]
    url = (f"{DEVICE_URL}/nowplaying?title={quote(title, safe='')}"
           f"&artist={quote(artist, safe='')}"
           f"&pos={int(pos)}&dur={int(dur)}&paused={int(paused)}")
    url += (f"&lyric={quote(lyric, safe='')}"
            f"&lyric2={quote(lyric2, safe='')}"
            f"&lyric3={quote(lyric3, safe='')}"
            f"&lt={int(lt)}"
            f"&lt2={int(lt2)}")
    try:
        device_post(url)
        shown = title or "— Not Playing —"
        timing = f" [{pos}/{dur}s]" if pos >= 0 and dur > 0 else ""
        state = " paused" if paused == 1 and title else ""
        print(f"now playing: {shown}" + (f" — {artist}" if artist else "") + timing + state)
    except Exception as e:
        print(f"nowplaying push failed: {e}", file=sys.stderr)


def base_result(**kw):
    """A push payload with every field defaulted; pollers fill what they have."""
    r = {"s": -1, "w": -1, "st": 0, "wt": 0,
         "sr": 0, "wr": 0, "stat": "", "bind": 0, "t": 0,
         "cpu": -1, "mem": -1, "disk": -1, "bat": -1,
         "sleep": POLL_INTERVAL}
    r.update(kw)
    return r


def poll_api_usage():
    """Make a 1-token request and read real server-side quota usage headers."""
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(API_BODY).encode(),
        headers={
            "content-type": "application/json",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Authorization": f"Bearer {get_token()}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            s, w = usage_from_headers(resp.headers)
            return base_result(s=s, w=w, **extra_from_headers(resp.headers))
    except (TimeoutError, socket.timeout) as e:
        raise TimeoutError("Anthropic API poll timed out after 30s") from e
    except urllib.error.URLError as e:
        if isinstance(e.reason, socket.timeout):
            raise TimeoutError("Anthropic API poll timed out after 30s") from e
        raise


def scan_local_usage():
    """Read Claude Code JSONL transcripts and return token totals for 5h and 7d."""
    now = datetime.now(timezone.utc)
    session_start = now - timedelta(hours=5)
    weekly_start = now - timedelta(days=7)
    latest_by_message = {}
    turns_without_id = []

    for base in PROJECT_DIRS:
        if not base.exists():
            continue
        for path in base.rglob("*.jsonl"):
            try:
                with path.open(encoding="utf-8", errors="replace") as f:
                    for line in f:
                        try:
                            record = json.loads(line)
                        except json.JSONDecodeError:
                            continue

                        parsed = turn_tokens(record)
                        if not parsed:
                            continue

                        ts, tokens, message_id = parsed
                        if ts < weekly_start:
                            continue

                        # Claude Code can write multiple streaming records for
                        # one message; keep the latest tally for that message.
                        if message_id:
                            latest_by_message[message_id] = (ts, tokens)
                        else:
                            turns_without_id.append((ts, tokens))
            except OSError as e:
                print(f"warning: cannot read {path}: {e}", file=sys.stderr)

    turns = list(latest_by_message.values()) + turns_without_id
    session_tokens = sum(tokens for ts, tokens in turns if ts >= session_start)
    weekly_tokens = sum(tokens for _ts, tokens in turns)
    return session_tokens, weekly_tokens


def percent(tokens, limit):
    if limit <= 0:
        return -1
    return min(100, round(tokens * 100 / limit))


def poll_local_usage():
    session_tokens, weekly_tokens = scan_local_usage()
    return base_result(
        s=percent(session_tokens, SESSION_TOKEN_LIMIT),
        w=percent(weekly_tokens, WEEKLY_TOKEN_LIMIT),
        st=session_tokens,
        wt=weekly_tokens,
    )


def poll_usage():
    if not USAGE_ENABLED:
        return base_result(s=0, w=0, stat="usage_off")
    if USAGE_SOURCE == "local":
        return poll_local_usage()
    if USAGE_SOURCE != "api":
        raise RuntimeError(
            "CLAWDMETER_USAGE_SOURCE must be 'off', 'api', 'server', 'headers', or 'local'"
        )
    return poll_api_usage()


def push(r):
    url = (f"{DEVICE_URL}/usage?s={r['s']}&w={r['w']}&st={r['st']}&wt={r['wt']}"
           f"&sr={r['sr']}&wr={r['wr']}&stat={urllib.parse.quote(r['stat'])}"
           f"&bind={r['bind']}&t={r['t']}"
           f"&cpu={r['cpu']}&mem={r['mem']}&disk={r['disk']}&bat={r['bat']}")
    device_post(url)


def device_post(url):
    """POST to the device with the shared retry/timeout policy."""
    last_error = None
    attempts = max(1, DEVICE_PUSH_ATTEMPTS)
    for attempt in range(1, attempts + 1):
        try:
            urllib.request.urlopen(
                urllib.request.Request(url, method="POST"),
                timeout=DEVICE_TIMEOUT,
            ).read()
            return
        except urllib.error.HTTPError:
            raise
        except (TimeoutError, socket.timeout) as e:
            last_error = e
        except urllib.error.URLError as e:
            last_error = e
            if not isinstance(e.reason, socket.timeout) and attempt == attempts:
                raise

        if attempt < attempts:
            time.sleep(min(2.0, 0.5 * attempt))

    if isinstance(last_error, (TimeoutError, socket.timeout)):
        raise TimeoutError(
            f"device push timed out after {DEVICE_TIMEOUT:g}s "
            f"({attempts} attempts): {url}"
        ) from last_error
    if isinstance(last_error, urllib.error.URLError):
        if isinstance(last_error.reason, socket.timeout):
            raise TimeoutError(
                f"device push timed out after {DEVICE_TIMEOUT:g}s "
                f"({attempts} attempts): {DEVICE_URL}/usage"
            ) from last_error
        raise last_error


def push_mac_only(reason):
    """Best-effort push of Mac metrics when Claude usage polling is unavailable."""
    r = base_result(stat=reason)
    r.update(mac_metrics())
    try:
        push(r)
        print(f"mac-only: {reason}  cpu={r['cpu']}% mem={r['mem']}% bat={r['bat']}%")
    except Exception as push_error:
        print(f"device push failed: {push_error}", file=sys.stderr)


def daily_fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "clawdmeter-daemon"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.load(resp)


def push_daily_images():
    """Resolve today's xkcd + NASA APOD metadata and push it to the device's
    /daily endpoint (see esp8266 daily_screen.cpp — the chip fetches the image
    itself over plain HTTP via the wsrv.nl proxy; it only needs URL + title).
    Returns seconds to wait before the next attempt."""
    args = {}
    try:
        c = daily_fetch_json("https://xkcd.com/info.0.json")
        args.update(comicimg=c["img"], comictitle=c.get("title", ""),
                    comicnum=f"#{c.get('num', '')}")
    except Exception as e:
        print(f"xkcd fetch failed: {e}", file=sys.stderr)
    try:
        a = daily_fetch_json(
            f"https://api.nasa.gov/planetary/apod?api_key={NASA_API_KEY}&thumbs=true")
        img = a.get("thumbnail_url") if a.get("media_type") == "video" else a.get("url")
        if img:
            args.update(apodimg=img, apodtitle=a.get("title", ""),
                        apoddate=a.get("date", ""))
    except Exception as e:
        print(f"apod fetch failed: {e}", file=sys.stderr)
    if not args:
        return DAILY_RETRY
    try:
        device_post(f"{DEVICE_URL}/daily?{urllib.parse.urlencode(args)}")
    except Exception as e:
        print(f"daily push failed: {e}", file=sys.stderr)
        return DAILY_RETRY
    print(f"daily: comic={args.get('comicnum', 'skipped')}  "
          f"apod={args.get('apoddate', 'skipped')}")
    # Partial success (one API down) retries sooner so the missing half fills in.
    return DAILY_INTERVAL if ("comicimg" in args and "apodimg" in args) else DAILY_RETRY


def poll_and_push_usage():
    """Poll Claude usage once and push it (with Mac metrics). Returns the number
    of seconds to wait before the next usage poll (normally POLL_INTERVAL, or a
    rate-limit backoff on 429)."""
    if not USAGE_ENABLED:
        return POLL_INTERVAL

    try:
        r = poll_usage()
        r.update(mac_metrics())
        push(r)
        if USAGE_SOURCE == "local":
            print(f"session={r['s']}% ({r['st']} tok)  weekly={r['w']}% ({r['wt']} tok)  "
                  f"cpu={r['cpu']}% mem={r['mem']}% bat={r['bat']}%")
        else:
            print(f"session={r['s']}%  weekly={r['w']}%  status={r['stat'] or '?'}  "
                  f"cpu={r['cpu']}% mem={r['mem']}% bat={r['bat']}%")
        return r["sleep"]
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("401 Unauthorized: run any Claude Code command to refresh login.",
                  file=sys.stderr)
            push_mac_only("claude_auth")
        elif e.code == 429:
            s, w = usage_from_headers(e.headers)
            sleep_for = retry_after_seconds(e.headers)
            if s >= 0 or w >= 0:
                r = base_result(s=s, w=w, **extra_from_headers(e.headers))
                r.update(mac_metrics())
                try:
                    push(r)
                except Exception as push_error:
                    print(f"device push failed: {push_error}", file=sys.stderr)
                print(f"rate limited: session={s}% weekly={w}% "
                      f"(retrying in {sleep_for}s)", file=sys.stderr)
            else:
                print(f"rate limited by Anthropic API "
                      f"(retrying in {sleep_for}s)", file=sys.stderr)
                push_mac_only("claude_rate_limited")
            return sleep_for
        else:
            print(f"API error {e.code}: {e.reason}", file=sys.stderr)
            push_mac_only("claude_http_error")
    except TimeoutError as e:
        print(f"timeout: {e}", file=sys.stderr)
        push_mac_only("claude_timeout")
    except urllib.error.URLError as e:
        print(f"network error: {e}", file=sys.stderr)
        push_mac_only("claude_network_error")
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        push_mac_only("claude_unavailable")
    return POLL_INTERVAL


def main():
    usage_text = f"usage source={USAGE_SOURCE} every {POLL_INTERVAL}s" if USAGE_ENABLED else "usage off"
    daily_text = f"daily images every {DAILY_INTERVAL // 3600}h" if DAILY_ENABLED else "daily images off"
    print(f"Clawdmeter daemon -> {DEVICE_URL}, {usage_text}, {daily_text}, "
          f"now-playing every {NOWPLAYING_TICK}s")
    # The song changes every few minutes, so we read it on a fast ~8s tick while
    # keeping the (costlier) Claude usage poll on its 60s cadence via a deadline.
    next_usage = 0.0 if USAGE_ENABLED else float("inf")
    next_daily = 0.0 if DAILY_ENABLED else float("inf")
    # monotonic time of the next due usage poll (0 = now; inf = disabled)
    # Seed with the "nothing playing" state so a daemon (re)start with no song
    # open sends no /nowplaying push (now-playing is web-only, but there's still
    # no reason to push an empty card on every launchd restart). The first real
    # song still differs from this and pushes.
    last_song = ("", "", -1, -1, -1)
    lyric_state = {}
    last_lyric_payload = ("", "", "", -1, -1)
    last_nowplaying_push = 0.0
    while True:
        now = time.monotonic()
        if now >= next_usage:
            next_usage = now + poll_and_push_usage()
        if now >= next_daily:
            next_daily = now + push_daily_images()

        last_title, last_artist, _last_pos, last_dur, last_paused = last_song
        song = normalize_nowplaying_timing(read_now_playing(), last_song)
        title, artist, pos, dur, paused = song
        lyrics = lyric_payload(title, artist, pos, dur, paused, now, lyric_state)
        state_changed = (
            title != last_title or
            artist != last_artist or
            dur != last_dur or
            paused != last_paused
        )
        lyric_changed = lyrics != last_lyric_payload
        resync_due = (
            bool(title) and pos >= 0 and dur > 0 and
            now - last_nowplaying_push >= NOWPLAYING_RESYNC
        )
        last_song = song
        if state_changed or lyric_changed or resync_due:
            push_now_playing(
                *song,
                lyric=lyrics[0],
                lyric2=lyrics[1],
                lyric3=lyrics[2],
                lt=lyrics[3],
                lt2=lyrics[4],
            )
            last_nowplaying_push = now
            last_lyric_payload = lyrics

        # Read fast (4s) while a song tab is open, slow when idle. Cap the sleep to the
        # next usage deadline so a long idle tick never delays the 60s usage poll.
        tick = NOWPLAYING_TICK if title else NOWPLAYING_IDLE_TICK
        time.sleep(max(0.0, min(tick, next_usage - time.monotonic(),
                                next_daily - time.monotonic())))


if __name__ == "__main__":
    main()

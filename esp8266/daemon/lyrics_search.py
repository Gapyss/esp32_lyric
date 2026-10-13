"""
lyrics_search.py — find lyrics on LRCLIB for the track YouTube Music is playing.

Stdlib only. One entry point:

    hit = find_lyrics(Track(title, artist, album, duration))
    if hit and hit.synced: ...   # LRC text, parse with the broker's existing parser
    elif hit and hit.plain: ...  # plain lyrics, no timings
    elif hit and hit.instrumental: ...
    else: ...                    # None = confidently not found (safe to negative-cache)

Network problems raise LrclibError (kind = "offline" | "rate_limited" | "failed"),
so the broker can retry later instead of caching a miss.

Principle: no lyrics beat the wrong song's lyrics. A record is accepted only when
  1. the title's identity matches after cleanup ("Song (Remastered 2011)" == "Song"),
  2. the version tags agree both ways (live, remix, acoustic, sped up, Thai ver., ...),
  3. at least one artist name is shared (channel name, credits, or "Artist - Song" in the title),
  4. the length fits: ±3 s for songs; for videos (MVs run long) the song may be up to
     VIDEO_MAX_EXTRA_S shorter than the video, never longer than it by more than 3 s.
"""

from __future__ import annotations

import json
import re
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

LRCLIB_BASE = "https://lrclib.net"
# LRCLIB asks clients to identify themselves. Put your project name / repo here.
USER_AGENT = "g4pys-lyrics-display/0.1"
SONG_TOLERANCE_S = 3.0
VIDEO_MAX_EXTRA_S = 90.0  # how much longer than the song an MV may be (intro/outro)
TIMEOUT_S = 10

# --------------------------------------------------------------------------- types


@dataclass
class Track:
    """What the broker reads from YouTube Music (navigator.mediaSession + <video>)."""

    title: str
    artist: str
    album: Optional[str] = None
    duration: Optional[float] = None  # seconds
    is_video: Optional[bool] = None   # None = guess: no album -> video (MV / upload)

    @property
    def video(self) -> bool:
        if self.is_video is not None:
            return self.is_video
        return not (self.album and self.album.strip())


@dataclass
class LyricsHit:
    synced: Optional[str]
    plain: Optional[str]
    instrumental: bool
    lrclib_id: Optional[int]
    track_name: str
    artist_name: str
    album_name: Optional[str]
    duration: Optional[float]
    matched_by: str  # which query found it, for logs
    record: dict = field(default_factory=dict, repr=False)  # the raw LRCLIB record

    @property
    def is_synced(self) -> bool:
        return bool(self.synced and self.synced.strip())


class LrclibError(Exception):
    def __init__(self, kind: str, detail: str = ""):
        super().__init__(f"{kind}: {detail}" if detail else kind)
        self.kind = kind  # "offline" | "rate_limited" | "failed"


HttpGet = Callable[[str], "tuple[int, str]"]
# (path, params) -> LRCLIB records; [] when LRCLIB says "not found". The daemon passes its own,
# which already retries Cloudflare 503s and honours track-change cancellation.
Fetch = Callable[[str, dict], "list[dict]"]

# --------------------------------------------------------------------------- text keys


def _strip_latin_accents(s: str) -> str:
    """Beyoncé -> Beyonce, but keep Thai vowel/tone marks (also combining marks)."""
    out = []
    for ch in unicodedata.normalize("NFKD", s):
        if unicodedata.category(ch) == "Mn" and out and out[-1].isascii():
            continue
        out.append(ch)
    return unicodedata.normalize("NFC", "".join(out))


def key(s: str) -> str:
    """Comparable form: casefolded, accents off (Latin only), letters/digits/marks only, no spaces."""
    s = _strip_latin_accents(unicodedata.normalize("NFKC", s or "")).casefold()
    s = s.replace("&", "and")
    return "".join(ch for ch in s if unicodedata.category(ch)[0] in "LNM")


# --------------------------------------------------------------------------- version tags

_VERSION_RULES: list[tuple[str, str]] = [
    ("live", r"\blive\b|\bconcert\b|แสดงสด|คอนเสิร์ต"),
    ("remix", r"\bremix(?:ed)?\b|\brmx\b|\brework\b|\bbootleg\b|\bflip\b|(?<!original )\bmix\b"),
    ("acoustic", r"\bacoustic\b|\bunplugged\b|อะคูสติก"),
    ("instrumental", r"\binstrumental\b|\binst\b|\bkaraoke\b|\boff ?vocal\b|\bminus ?one\b|คาราโอเกะ"),
    ("sped_up", r"\bsped ?up\b|\bspeed ?up\b|\bnightcore\b"),
    ("slowed", r"\bslowed\b|\breverb\b"),
    ("cover", r"\bcover\b"),
    ("demo", r"\bdemo\b"),
    ("acapella", r"\ba ?cappella\b|\bacapella\b"),
    ("extended", r"\bextended\b"),
]
_VERSION_RES = [(tag, re.compile(rx, re.I)) for tag, rx in _VERSION_RULES]

# "<word> ver." / "<word> version" -> ver:<word>, unless the word is neutral packaging.
_VER_WORD_RE = re.compile(r"(?<![\w'’])([^\W\d_]{2,})\.?\s+ver(?:sion|s)?\b\.?", re.I)
_NEUTRAL_VER_WORDS = {
    "album", "original", "single", "radio", "full", "short", "official", "mv", "tv", "music",
    "video", "explicit", "clean", "remastered", "remaster", "new", "lyric", "lyrics", "audio",
    "extended", "edit", "the", "a", "studio",
}
_LANG_ALIASES = {
    "eng": "english", "en": "english", "jp": "japanese", "jpn": "japanese", "kr": "korean",
    "kor": "korean", "cn": "chinese", "chn": "chinese", "mandarin": "chinese", "th": "thai",
}
# Thai "เวอร์ชั่น/เวอร์ชัน <x>"
_THAI_VER_RE = re.compile(r"เวอร์ชั่?น\s*(\S+)")


def version_tags(text: str) -> set[str]:
    tags = {tag for tag, rx in _VERSION_RES if rx.search(text)}
    for m in _VER_WORD_RE.finditer(text):
        w = m.group(1).casefold()
        if w not in _NEUTRAL_VER_WORDS:
            tags.add("ver:" + _LANG_ALIASES.get(w, w))
    for m in _THAI_VER_RE.finditer(text):
        tags.add("ver:" + key(m.group(1)))
    return tags


# A dash suffix is a version only when it reads like one ("Live at X", "Acoustic", "Club Mix"),
# so "Artist - Live Forever" keeps its title.
_VERSION_START_RE = re.compile(
    r"(?:live(?:\s+(?:at|from|in|on|version|ver)\b|$)|acoustic|unplugged|instrumental|karaoke|sped|speed|"
    r"slowed|nightcore|demo|cover|acapella|a cappella|extended|remix|rmx|(?:\w+\s+)?mix$|\w+\s+remix|"
    r"\w+\.?\s+ver(?:sion)?\.?$|แสดงสด|อะคูสติก|คาราโอเกะ)",
    re.I,
)

# Things in a title that never change the words: drop them.
_PACKAGING_RE = re.compile(
    r"remaster|radio edit|\bedit\b|single version|album version|\bmono\b|\bstereo\b|bonus|"
    r"\bfrom\b|official|\bm/?v\b|music video|lyric|\baudio\b|visuali[sz]er|\bhd\b|\bhq\b|\b4k\b|"
    r"\bprod\b|\bost\b|soundtrack|explicit|\bclean\b|version|\bver\b|เนื้อเพลง|ซับ|เพลงประกอบ|"
    r"\bfeat\b|\bft\b|featuring",
    re.I,
)
_BRACKET_RE = re.compile(r"[\(\[\{【「『《〈（［](.*?)[\)\]\}】」』》〉）］]")
_SEP_RE = re.compile(r"\s+[-–—|｜]\s+")
_FEAT_RE = re.compile(r"\s*\b(?:feat\.?|ft\.?|featuring)\s+(.+)$", re.I)
_TRAILING_JUNK_RE = re.compile(
    r"\s*(?:official\s*(?:music\s*|lyrics?\s*)?video|music\s*video|lyrics?\s*video|(?:official\s*)?m/?v|"
    r"official\s*(?:audio|lyrics?)|visuali[sz]er)\s*$", re.I
)


@dataclass
class ParsedTitle:
    clean: str                      # human form, used as the search query
    ident: str                      # key(clean), compared for identity
    tags: frozenset[str]
    artists_in_title: list[str] = field(default_factory=list)  # feat. / "Artist - Song" hints


_QUOTES = " \"'“”‘’「」"


def _strip_trailing_junk(main: str) -> str:
    prev = None
    while prev != main:
        prev, main = main, _TRAILING_JUNK_RE.sub("", main)
    return main


def _unbracket(title: str) -> "tuple[str, set[str], list[str]]":
    """Drop bracketed segments (keeping their version tags and feat. names) and trailing 'Official MV'."""
    title = unicodedata.normalize("NFKC", title or "").strip()
    tags: set[str] = set()
    hints: list[str] = []
    for seg in _BRACKET_RE.findall(title):
        tags |= version_tags(seg)
        m = re.match(r"\s*(?:feat\.?|ft\.?|featuring|with)\s+(.+)", seg, re.I)
        if m:
            hints.append(m.group(1))
    main = _strip_trailing_junk(_BRACKET_RE.sub(" ", title))
    return re.sub(r"\s+", " ", main).strip(), tags, hints


def _is_packaging(part: str) -> bool:
    """'2011 Remaster', 'Radio Edit', 'From "Frozen"' -> True; "'How You Like That' M/V" -> False."""
    p = part.strip(_QUOTES)
    if _PACKAGING_RE.match(p):
        return True
    if not _PACKAGING_RE.search(p):
        return False  # "2002" alone is a title, not a year stamp
    rest = _PACKAGING_RE.sub(" ", p)
    return not any(ch.isalpha() for ch in rest)


def parse_title(title: str) -> ParsedTitle:
    # 1. bracketed segments: harvest tags and feat. names, then drop them.
    main, tags, hints = _unbracket(title)

    # 2. "Song - Live at X" / "Song - 2011 Remaster": a trailing part that is only packaging/version.
    while True:
        parts = _SEP_RE.split(main)
        if len(parts) < 2:
            break
        last = parts[-1]
        last_tags = version_tags(last)
        if (last_tags and _VERSION_START_RE.match(last.strip(_QUOTES))) or _is_packaging(last):
            tags |= last_tags
            main = main[: main.rfind(last)].rstrip(" -–—|｜")
        else:
            break

    # 3. bare "feat. X" and trailing "Official MV" outside brackets.
    m = _FEAT_RE.search(main)
    if m:
        hints.append(m.group(1))
        main = main[: m.start()]
    main = _strip_trailing_junk(main)

    main = re.sub(r"\s+", " ", main).strip(" -–—|｜" + _QUOTES)
    return ParsedTitle(clean=main, ident=key(main), tags=frozenset(tags), artists_in_title=hints)


# --------------------------------------------------------------------------- artists

_CHANNEL_JUNK_RE = re.compile(r"\s*(?:-\s*topic|vevo|\bofficial\b|\bchannel\b)\s*", re.I)
_ARTIST_SPLIT_RE = re.compile(r"\s*(?:,|&|/|;|\+|×|、|\bx\b|\bfeat\.?|\bft\.?|\bfeaturing\b|และ)\s*", re.I)


def artist_parts(s: str) -> list[str]:
    """Display names: 'A feat. B & C' -> ['A feat. B & C', 'A', 'B', 'C'] (channel junk removed)."""
    s = _CHANNEL_JUNK_RE.sub(" ", unicodedata.normalize("NFKC", s or "")).strip()
    if not s:
        return []
    out = [s] + [p.strip() for p in _ARTIST_SPLIT_RE.split(s) if p and p.strip()]
    seen, uniq = set(), []
    for p in out:
        k = key(p)
        if len(k) >= 2 and k not in seen:
            seen.add(k)
            uniq.append(p)
    return uniq


def artist_keys(*names: str) -> set[str]:
    keys = set()
    for n in names:
        for p in artist_parts(n):
            k = key(p)
            keys.add(k[3:] if k.startswith("the") and len(k) > 5 else k)
    return keys


# --------------------------------------------------------------------------- request variants


@dataclass
class Want:
    title: ParsedTitle
    artist_query: str          # primary artist to put in the query
    artists: set[str]          # every key that counts as "the same artist"
    label: str                 # for logs


def wants_for(track: Track) -> list[Want]:
    """The plain reading, plus 'Artist - Song' / 'Song - Artist' readings of a video title."""
    base = parse_title(track.title)
    channel = artist_parts(track.artist)
    primary = channel[1] if len(channel) > 1 else (channel[0] if channel else track.artist)
    common = artist_keys(track.artist, *base.artists_in_title)
    wants = [Want(base, primary, common, "title")]

    raw, bracket_tags, _ = _unbracket(track.title)
    parts = _SEP_RE.split(raw)
    if len(parts) == 2 and all(key(p) for p in parts):
        left, right = (p.strip(_QUOTES) for p in parts)
        readings = [(right, left, "artist-song"), (left, right, "song-artist")]
        if artist_keys(right) & common and not artist_keys(left) & common:
            readings.reverse()  # the right side is the channel/artist -> "Song - Artist"
        known = bool((artist_keys(left) | artist_keys(right)) & common)
        variants = []
        for song, who, label in readings:
            side = parse_title(song)
            t = ParsedTitle(clean=side.clean, ident=side.ident, tags=frozenset(side.tags | bracket_tags),
                            artists_in_title=base.artists_in_title + side.artists_in_title + [who])
            variants.append(Want(t, artist_parts(who)[0] if artist_parts(who) else who,
                                 common | artist_keys(who), label))
        # One side is the artist we already know: that reading is the likeliest, ask it first.
        wants = [variants[0], wants[0], variants[1]] if known else wants + variants
    return wants


# --------------------------------------------------------------------------- matching


def duration_ok(track: Track, cand_dur: Optional[float]) -> bool:
    if not track.duration or not cand_dur:
        return True  # unknown on either side skips only this check
    if track.video:
        extra = track.duration - cand_dur  # MV length minus song length
        return -SONG_TOLERANCE_S <= extra <= VIDEO_MAX_EXTRA_S
    return abs(track.duration - cand_dur) <= SONG_TOLERANCE_S


def matches(want: Want, track: Track, rec: dict) -> bool:
    cand = parse_title(rec.get("trackName") or "")
    if not want.title.ident or cand.ident != want.title.ident:
        return False
    if set(cand.tags) != set(want.title.tags):
        return False
    if not (artist_keys(rec.get("artistName") or "") & want.artists):
        return False
    return duration_ok(track, rec.get("duration"))


def _has_words(rec: dict) -> bool:
    return bool((rec.get("syncedLyrics") or "").strip() or (rec.get("plainLyrics") or "").strip())


def _is_synced(rec: dict) -> bool:
    return bool((rec.get("syncedLyrics") or "").strip())


def _rank(track: Track, rec: dict):
    d = rec.get("duration")
    gap = abs(track.duration - d) if (track.duration and d) else float("inf")
    if track.video and track.duration and d:
        gap = abs(track.duration - d) if d > track.duration else (track.duration - d) / 10
    same_album = bool(track.album and rec.get("albumName")
                      and key(track.album) == key(rec["albumName"]))
    return (0 if _is_synced(rec) else 1, gap, 0 if same_album else 1)


# --------------------------------------------------------------------------- http


def urllib_get(url: str) -> "tuple[int, str]":
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
            return r.status, r.read(2 * 1024 * 1024).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(64 * 1024).decode("utf-8", "replace")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise LrclibError("offline", str(e)) from e


def _fetch(http: HttpGet, path: str, params: dict) -> list[dict]:
    q = urllib.parse.urlencode({k: v for k, v in params.items() if v not in (None, "")})
    status, body = http(f"{LRCLIB_BASE}{path}?{q}")
    if status == 404:
        return []
    if status == 429:
        raise LrclibError("rate_limited")
    if not 200 <= status < 300:
        raise LrclibError("failed", f"HTTP {status}")
    try:
        data = json.loads(body)
    except ValueError as e:
        raise LrclibError("failed", "bad JSON") from e
    if isinstance(data, dict):
        return [data]
    return [r for r in data if isinstance(r, dict)] if isinstance(data, list) else []


# --------------------------------------------------------------------------- search


def _hit(rec: dict, how: str) -> LyricsHit:
    return LyricsHit(
        synced=rec.get("syncedLyrics") or None,
        plain=rec.get("plainLyrics") or None,
        instrumental=bool(rec.get("instrumental")),
        lrclib_id=rec.get("id"),
        track_name=rec.get("trackName") or "",
        artist_name=rec.get("artistName") or "",
        album_name=rec.get("albumName"),
        duration=rec.get("duration"),
        matched_by=how,
        record=rec,
    )


def _queries(track: Track, wants: list[Want]) -> Iterable[tuple[str, str, dict, list[Want]]]:
    """(label, path, params, wants it may satisfy), cheapest and most precise first."""
    base = wants[0]
    if not track.video and track.duration:
        yield ("get", "/api/get", {
            "track_name": track.title, "artist_name": track.artist,
            "album_name": track.album, "duration": round(track.duration),
        }, [base])
    seen = set()
    for w in wants:
        qk = (key(w.title.clean), key(w.artist_query))
        if qk in seen or not w.title.clean:
            continue
        seen.add(qk)
        yield (f"search:{w.label}", "/api/search",
               {"track_name": w.title.clean, "artist_name": w.artist_query}, [w])
    # Last resort: title only (label channel names, romanised vs Thai artist names...).
    # Artist must still match something we know, so this stays safe.
    yield ("search:title-only", "/api/search", {"track_name": base.title.clean}, wants)


def find_lyrics(
    track: Track,
    http: HttpGet = urllib_get,
    max_requests: int = 5,
    *,
    fetch: Optional[Fetch] = None,
    require_synced: bool = False,
) -> Optional[LyricsHit]:
    """Best LRCLIB record for the track, or None when nothing is confidently the same song.

    require_synced: plain-only records count as "not found" (a display that scrolls in time
    has nothing to do with them); instrumental records are still accepted.
    """
    if not (track.title or "").strip() or not (track.artist or "").strip():
        return None
    if fetch is None:
        fetch = lambda path, params: _fetch(http, path, params)  # noqa: E731
    wants = wants_for(track)
    best: Optional[tuple[tuple, dict, str]] = None  # (rank, record, how)
    seen_ids: set = set()

    for n, (label, path, params, ws) in enumerate(_queries(track, wants)):
        if n >= max_requests:
            break
        for rec in fetch(path, params):
            if not isinstance(rec, dict):
                continue
            rid = rec.get("id")
            if rid is not None and rid in seen_ids:
                continue
            seen_ids.add(rid)
            if not (_has_words(rec) or rec.get("instrumental")):
                continue
            if require_synced and not (_is_synced(rec) or rec.get("instrumental")):
                continue
            if not any(matches(w, track, rec) for w in ws):
                continue
            if rec.get("instrumental") and not _has_words(rec):
                return _hit(rec, label)  # LRCLIB says no vocals: believe it
            r = _rank(track, rec)
            if best is None or r < best[0]:
                best = (r, rec, label)
        if best and _is_synced(best[1]):
            break  # synced and confident: stop asking
    return _hit(best[1], best[2]) if best else None


if __name__ == "__main__":  # quick manual check: python lyrics_search.py "Title" "Artist" [album] [seconds]
    import sys

    # Windows writes redirected stdout in the locale code page (cp1252), which can't hold Thai.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore[union-attr]
        except (AttributeError, ValueError):
            pass

    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit('usage: lyrics_search.py "Title" "Artist" [album] [seconds]')
    t = Track(a[0], a[1], a[2] if len(a) > 2 and a[2] else None, float(a[3]) if len(a) > 3 else None)
    try:
        h = find_lyrics(t)
    except LrclibError as e:
        sys.exit(f"lrclib error ({e.kind}): {e}")
    if not h:
        print("not found")
    else:
        print(f"{h.track_name} — {h.artist_name} [{h.album_name}] {h.duration}s "
              f"synced={h.is_synced} via {h.matched_by}")
        print((h.synced or h.plain or "(instrumental)")[:400])

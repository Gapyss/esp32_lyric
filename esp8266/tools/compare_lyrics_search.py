"""Compare the old LRCLIB resolver with lyrics_search on real tracks.

    # every track you've played (the daemon's cache), newest first:
    python3 esp8266/tools/compare_lyrics_search.py --db ~/.g4pys/lyrics-display.sqlite3 --limit 40
    # or a JSON list of {"title", "artist", "album", "duration"}:
    python3 esp8266/tools/compare_lyrics_search.py --tracks tracks.json

Both resolvers run live against lrclib.net, and the tool prints one line per track plus a summary.
OLD is the resolver before PR #15: the exact /api/get, then any synced /api/search hit within ±3 s.
It never writes to the cache. Runs on macOS and Windows.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "daemon"))
import lyrics_search  # noqa: E402

UA = "g4pys-lyrics-display/0.1 (compare_lyrics_search)"


def lrclib(path: str, params: dict) -> list[dict]:
    q = urllib.parse.urlencode({k: v for k, v in params.items() if v not in ("", None, 0)})
    req = urllib.request.Request(f"https://lrclib.net{path}?{q}", headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                data = json.loads(r.read().decode("utf-8"))
            return [data] if isinstance(data, dict) else [d for d in data if isinstance(d, dict)]
        except urllib.error.HTTPError as e:
            if e.code in (400, 404):
                return []
            if e.code in (429, 502, 503, 504) and attempt < 2:
                time.sleep(2)
                continue
            raise
        except (urllib.error.URLError, TimeoutError):
            if attempt < 2:
                time.sleep(2)
                continue
            raise
    return []


def old_resolver(t: dict) -> dict | None:
    """The pre-PR fetch_lrclib, returning the chosen record."""
    params = {"track_name": t["title"], "artist_name": t["artist"], "album_name": t.get("album") or "",
              "duration": int(t.get("duration") or 0)}
    for rec in lrclib("/api/get", params):
        if (rec.get("syncedLyrics") or "").strip() or rec.get("instrumental"):
            return rec
    dur = float(t.get("duration") or 0)
    best = None
    for rec in lrclib("/api/search", params):
        if not (rec.get("syncedLyrics") or "").strip():
            continue
        d = float(rec.get("duration") or 0)
        dist = abs(d - dur) if dur > 0 and d > 0 else 0
        if dur > 0 and dist > 3:
            continue
        if best is None or dist < best[0]:
            best = (dist, rec)
    return best[1] if best else None


def new_resolver(t: dict) -> tuple[dict | None, str]:
    hit = lyrics_search.find_lyrics(
        lyrics_search.Track(t["title"], t["artist"], t.get("album") or None, float(t["duration"]) if t.get("duration") else None),
        fetch=lrclib, max_requests=4, require_synced=True,
    )
    return (hit.record, hit.matched_by) if hit else (None, "")


def describe(rec: dict | None) -> str:
    if not rec:
        return "-"
    return f"#{rec.get('id')} {rec.get('trackName')!r} / {rec.get('artistName')!r} {rec.get('duration')}s"


def tracks_from_db(path: Path, limit: int) -> list[dict]:
    con = sqlite3.connect(str(path))
    try:
        rows = con.execute(
            "SELECT title, artist, album, duration_sec FROM lyrics WHERE source = 'lrclib' AND title != ''"
            " ORDER BY fetched_at DESC LIMIT ?", (limit,)
        ).fetchall()
    finally:
        con.close()
    return [{"title": r[0], "artist": r[1] or "", "album": r[2] or "", "duration": r[3] or 0} for r in rows]


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore[union-attr]
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--db", type=Path, help="the daemon's lyrics cache (SQLite)")
    src.add_argument("--tracks", type=Path, help="JSON list of tracks")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--json-out", type=Path, help="also write the per-track results here")
    args = ap.parse_args()

    tracks = tracks_from_db(args.db.expanduser(), args.limit) if args.db else \
        json.loads(args.tracks.read_text(encoding="utf-8"))[: args.limit]

    counts = {"same": 0, "both_miss": 0, "new_only": 0, "old_only": 0, "different": 0, "error": 0}
    results = []
    for i, t in enumerate(tracks, 1):
        try:
            old = old_resolver(t)
            new, how = new_resolver(t)
        except Exception as e:  # network trouble: report and keep going
            counts["error"] += 1
            print(f"{i:3} ERROR      {t['title']!r} / {t['artist']!r}: {e}")
            continue
        if old and new:
            verdict = "same" if old.get("id") == new.get("id") else "different"
        elif new:
            verdict = "new_only"
        elif old:
            verdict = "old_only"
        else:
            verdict = "both_miss"
        counts[verdict] += 1
        mode = "song" if t.get("album") else "video"
        print(f"{i:3} {verdict:10} [{mode}] {t['title']!r} / {t['artist']!r} ({t.get('duration') or '?'}s)")
        if verdict != "both_miss":
            print(f"      old: {describe(old)}")
            print(f"      new: {describe(new)}{'  via ' + how if how else ''}")
        results.append({"track": t, "verdict": verdict, "old": describe(old), "new": describe(new), "via": how})
        time.sleep(0.3)  # be gentle with lrclib

    total = sum(counts.values())
    print("\nsummary over", total, "tracks:")
    for k, v in counts.items():
        print(f"  {k:10} {v}")
    print("\n  same       both picked the same record")
    print("  different  both found lyrics, different records: check which is right")
    print("  new_only   only the new search found lyrics")
    print("  old_only   only the old one did: the new rules refused it (right if the old pick was wrong)")
    if args.json_out:
        args.json_out.write_text(json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())

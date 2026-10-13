"""Offline tests for esp8266/daemon/lyrics_search.py — a fake LRCLIB answers from an in-memory table.

Run from the repo root: python3 -m unittest discover -s esp8266/tests -t .
"""

import importlib.util
import json
import os
import subprocess
import sys
import unittest
import urllib.parse
from pathlib import Path

# Loaded by file path, like the daemon tests, so it can't collide with anything else on sys.path.
_PATH = Path(__file__).resolve().parents[1] / "daemon" / "lyrics_search.py"
_spec = importlib.util.spec_from_file_location("esp8266_lyrics_search", _PATH)
assert _spec is not None and _spec.loader is not None
_mod = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _mod
_spec.loader.exec_module(_mod)

LrclibError, Track, artist_keys, find_lyrics = _mod.LrclibError, _mod.Track, _mod.artist_keys, _mod.find_lyrics
key, parse_title, version_tags = _mod.key, _mod.parse_title, _mod.version_tags

LRC = "[00:01.00]first line\n[00:05.00]second line"


def rec(id, track, artist, dur, album=None, synced=LRC, plain="first line\nsecond line", instrumental=False):
    return {"id": id, "trackName": track, "artistName": artist, "albumName": album, "duration": dur,
            "syncedLyrics": synced, "plainLyrics": plain, "instrumental": instrumental}


class FakeLrclib:
    """/api/get: exact-ish (title, artist, album, ±2 s). /api/search: loose substring on keys."""

    def __init__(self, records, status=None):
        self.records, self.status, self.calls = records, status, []

    def __call__(self, url):
        self.calls.append(url)
        if self.status:
            return self.status, ""
        u = urllib.parse.urlparse(url)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        if u.path == "/api/get":
            for r in self.records:
                if (key(r["trackName"]) == key(q["track_name"]) and key(r["artistName"]) == key(q["artist_name"])
                        and ("album_name" not in q or key(r["albumName"] or "") == key(q["album_name"]))
                        and abs(r["duration"] - float(q.get("duration", r["duration"]))) <= 2):
                    return 200, json.dumps(r)
            return 404, '{"name":"TrackNotFound"}'
        t = key(q.get("track_name", ""))
        a = key(q.get("artist_name", ""))
        hits = [r for r in self.records if t and t[:6] in key(r["trackName"]) and (not a or a[:4] in key(r["artistName"]))]
        return 200, json.dumps(hits)


class ParseTests(unittest.TestCase):
    def test_packaging_is_dropped(self):
        for t in ["Yellow (Remastered 2011)", "Yellow - 2011 Remaster", "Yellow [Official Music Video]",
                  "Yellow (Official MV)", "Yellow Official Video", "Yellow (feat. Someone)", "Yellow | Official Audio"]:
            p = parse_title(t)
            self.assertEqual(p.ident, "yellow", t)
            self.assertEqual(p.tags, frozenset(), t)

    def test_versions_are_tags(self):
        self.assertEqual(parse_title("Song (Live at Wembley)").tags, {"live"})
        self.assertEqual(parse_title("Song - Acoustic").tags, {"acoustic"})
        self.assertEqual(parse_title("Song (Sped Up)").tags, {"sped_up"})
        self.assertEqual(parse_title("Song (Thai Ver.)").tags, {"ver:thai"})
        self.assertEqual(parse_title("Song (Eng Version)").tags, {"ver:english"})
        self.assertEqual(parse_title("Song (Album Version)").tags, frozenset())
        self.assertEqual(version_tags("Original Mix"), set())
        self.assertEqual(version_tags("Club Mix"), {"remix"})

    def test_dash_suffix_that_is_a_title_is_kept(self):
        self.assertEqual(parse_title("Oasis - Live Forever").tags, frozenset())
        self.assertEqual(parse_title("Anne-Marie - 2002").ident, key("Anne-Marie 2002"))
        self.assertEqual(parse_title("Song - 2011 Remaster").ident, "song")
        self.assertEqual(parse_title("Love Story (Taylor's Version)").tags, frozenset())

    def test_words_inside_the_title_are_not_tags(self):
        self.assertEqual(parse_title("Live Your Life").tags, frozenset())
        self.assertEqual(parse_title("Live Your Life").ident, "liveyourlife")

    def test_thai_marks_are_kept(self):
        self.assertNotEqual(key("ไก่"), key("ไก"))
        self.assertEqual(key("ทางของฝุ่น"), key("ทาง ของ ฝุ่น"))
        self.assertEqual(parse_title("ทางของฝุ่น [Official MV]").ident, key("ทางของฝุ่น"))

    def test_latin_accents(self):
        self.assertEqual(key("Beyoncé"), key("Beyonce"))

    def test_artist_keys(self):
        self.assertIn(key("Coldplay"), artist_keys("ColdplayVEVO"))
        self.assertIn(key("Coldplay"), artist_keys("Coldplay - Topic"))
        self.assertTrue({key("Ann"), key("Bee")} <= artist_keys("Ann feat. Bee"))
        self.assertIn(key("Beatles"), artist_keys("The Beatles"))


class SearchTests(unittest.TestCase):
    def test_song_mode_exact_get(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 266, album="Parachutes")])
        hit = find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 267), api)
        self.assertEqual(hit.lrclib_id, 1)
        self.assertTrue(hit.is_synced)
        self.assertEqual(hit.matched_by, "get")
        self.assertEqual(len(api.calls), 1)

    def test_plain_exact_gives_way_to_synced_upload(self):
        api = FakeLrclib([
            rec(1, "Yellow", "Coldplay", 266, album="Parachutes", synced=None),
            rec(2, "Yellow", "Coldplay", 267, album="Yellow - Single"),
        ])
        hit = find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), api)
        self.assertEqual(hit.lrclib_id, 2)

    def test_plain_kept_when_nothing_synced(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 266, album="Parachutes", synced=None)])
        hit = find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), api)
        self.assertEqual(hit.lrclib_id, 1)
        self.assertFalse(hit.is_synced)
        self.assertTrue(hit.plain)

    def test_other_version_is_refused(self):
        api = FakeLrclib([
            rec(1, "Yellow (Live)", "Coldplay", 268),
            rec(2, "Yellow - Acoustic", "Coldplay", 266),
            rec(3, "Yellow (Remix)", "Coldplay", 265),
        ])
        self.assertIsNone(find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), api))

    def test_live_request_takes_live_record(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 266), rec(2, "Yellow (Live)", "Coldplay", 300)])
        hit = find_lyrics(Track("Yellow - Live", "Coldplay", "Live 2003", 301), api)
        self.assertEqual(hit.lrclib_id, 2)

    def test_remastered_title_matches_plain_record(self):
        api = FakeLrclib([rec(1, "Here Comes the Sun - Remastered 2009", "The Beatles", 185)])
        hit = find_lyrics(Track("Here Comes The Sun (2019 Mix)", "The Beatles", "Abbey Road", 186), api)
        # "2019 Mix" is a remix tag on purpose: a new mix can move the timings.
        self.assertIsNone(hit)
        hit = find_lyrics(Track("Here Comes The Sun (Remastered)", "Beatles", "Abbey Road", 186), api)
        self.assertEqual(hit.lrclib_id, 1)

    def test_song_mode_length_off_by_10s_is_refused(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 256)])
        self.assertIsNone(find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), api))

    def test_video_artist_dash_song_from_label_channel(self):
        api = FakeLrclib([rec(7, "Yellow", "Coldplay", 266)])
        t = Track("Coldplay - Yellow (Official Video)", "Some Label Records", None, 290)  # MV 24 s longer
        hit = find_lyrics(t, api)
        self.assertEqual(hit.lrclib_id, 7)
        self.assertLessEqual(len(api.calls), 5)

    def test_video_thai_song_dash_artist(self):
        api = FakeLrclib([rec(9, "ทางของฝุ่น", "อะตอม ชนกันต์", 245)])
        t = Track("ทางของฝุ่น - อะตอม ชนกันต์ [Official MV]", "GMM Label Official", None, 262)
        self.assertEqual(find_lyrics(t, api).lrclib_id, 9)

    def test_video_quoted_title_with_mv_suffix(self):
        api = FakeLrclib([rec(4, "How You Like That", "BLACKPINK", 181)])
        hit = find_lyrics(Track("BLACKPINK - 'How You Like That' M/V", "BLACKPINK", None, 184), api)
        self.assertEqual(hit.lrclib_id, 4)
        self.assertEqual(hit.matched_by, "search:artist-song")

    def test_video_title_that_looks_like_a_version(self):
        api = FakeLrclib([rec(5, "Live Forever", "Oasis", 276), rec(6, "Live Forever (Live)", "Oasis", 290)])
        hit = find_lyrics(Track("Oasis - Live Forever (Official Video)", "OasisVEVO", None, 280), api)
        self.assertEqual(hit.lrclib_id, 5)

    def test_video_song_cannot_be_longer_than_the_video(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 300)])
        self.assertIsNone(find_lyrics(Track("Coldplay - Yellow (Official Video)", "Coldplay", None, 266), api))

    def test_video_prefers_the_song_inside_the_mv(self):
        api = FakeLrclib([rec(1, "Yellow", "Coldplay", 200), rec(2, "Yellow", "Coldplay", 266)])
        hit = find_lyrics(Track("Yellow (Official Video)", "Coldplay", None, 280), api)
        self.assertEqual(hit.lrclib_id, 2)

    def test_unrelated_artist_is_refused(self):
        api = FakeLrclib([rec(1, "Yellow", "Someone Else", 266)])
        self.assertIsNone(find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), api))

    def test_thai_version_tag(self):
        api = FakeLrclib([rec(1, "Song", "Artist", 200)])
        self.assertIsNone(find_lyrics(Track("Song (Thai Ver.)", "Artist", "Album", 200), api))

    def test_instrumental_is_believed(self):
        api = FakeLrclib([rec(1, "Theme", "Composer", 180, synced=None, plain=None, instrumental=True)])
        hit = find_lyrics(Track("Theme", "Composer", "OST", 180), api)
        self.assertTrue(hit.instrumental)

    def test_errors_raise(self):
        with self.assertRaises(LrclibError) as e:
            find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), FakeLrclib([], status=429))
        self.assertEqual(e.exception.kind, "rate_limited")
        with self.assertRaises(LrclibError) as e:
            find_lyrics(Track("Yellow", "Coldplay", "Parachutes", 266), FakeLrclib([], status=500))
        self.assertEqual(e.exception.kind, "failed")

    def test_blank_input(self):
        self.assertIsNone(find_lyrics(Track("", "Coldplay"), FakeLrclib([])))


class CliTests(unittest.TestCase):
    # Windows writes a redirected stdout in cp1252, which can't hold Thai. Force that
    # code page on every OS so a missing reconfigure() fails here, not on someone's PC.
    SCRIPT = (
        "import json, runpy, sys, urllib.request\n"
        "class R:\n"
        "    status = 200\n"
        "    def __init__(self, b): self.b = b\n"
        "    def read(self, n=-1): return self.b\n"
        "    def __enter__(self): return self\n"
        "    def __exit__(self, *a): pass\n"
        "rec = [{'id': 1, 'trackName': 'ทางของฝุ่น', 'artistName': 'อะตอม ชนกันต์', 'duration': 245.0,"
        " 'syncedLyrics': '[00:01.00]ทดสอบ'}]\n"
        "urllib.request.urlopen = lambda req, timeout=0: R(json.dumps(rec).encode())\n"
        "sys.argv = ['lyrics_search.py', 'ทางของฝุ่น', 'อะตอม ชนกันต์']\n"
        "runpy.run_path(sys.argv_path, run_name='__main__')\n"
    )

    def test_cli_prints_thai_into_a_cp1252_pipe(self):
        script = self.SCRIPT.replace("sys.argv_path", repr(str(_PATH)))
        env = dict(os.environ, PYTHONIOENCODING="cp1252")
        out = subprocess.run([sys.executable, "-c", script], capture_output=True, env=env, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr.decode("utf-8", "replace"))
        text = out.stdout.decode("utf-8")
        self.assertIn("ทางของฝุ่น", text)
        self.assertIn("synced=True", text)


if __name__ == "__main__":
    unittest.main()

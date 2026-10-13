# esp8266/ — notes for Claude Code

## Platforms: macOS **and** Windows
Everything on the PC side (`daemon/`, `tools/`, `browser_extension/`) must run on both macOS and Windows.
- Python stdlib first; no POSIX-only calls (signals, `fcntl`, hard-coded `/` paths) without a Windows branch.
- Use `pathlib`, and open text files with `encoding="utf-8"`.
- Thai text: Windows writes redirected stdout in cp1252, so any entry point that prints must
  `reconfigure(encoding="utf-8", errors="backslashreplace")` like `main()` in the daemon.
- macOS renders with Core Text; Windows with the Pillow renderer (`daemon/requirements-windows.txt`).
- CI (`.github/workflows/esp8266-tests.yml`) runs the suite on `macos-latest` and `windows-latest`;
  a change isn't done until both are green.

## Tests
`python3 -m unittest discover -s esp8266/tests -t .` from the repo root.

## Lyrics search
`daemon/lyrics_search.py` decides which LRCLIB record is the song playing. Its rules are strict
on purpose (no lyrics beat another recording's). Before loosening one, add the real track that
failed as a case in `tests/test_lyrics_search.py`.

## Fork
`esp8266/daemon/lyrics_display_daemon.py` is a fork of `daemon/` (ESP32); fixes that matter to
both boards must be applied twice (see README "This daemon is a fork").

#!/usr/bin/env python3
"""Lyrics display daemon for the 240x240 ESP8266 SmallTV board, standalone.

Runs the same daemon as :mod:`daemon.lyrics_display_daemon` with three defaults
flipped, which is what makes the ESP8266 independent of the ESP32:

* ``--boards esp8266`` -- only the 240x240 profile is registered, so a board
  handshake this process cannot parse falls back to a geometry that fits the
  panel instead of a 400x300 frame.
* ``--insecure`` -- no daemon identity, so boards connect over unauthenticated
  proto=1. The ESP8266 lacks the heap to buffer-and-verify a SEC2 record on top
  of its framebuffer. Previously this flag was process-wide and dragged the
  ESP32 down with it; now it stops at this process.
* ``--relay-upstream`` -- the browser extension binds to one daemon only, so
  this one mirrors that daemon's feed over ``/relay`` instead of listening for
  the extension itself.

mDNS is off by default too: this board has no mDNS client. It learns the Mac's
IP from an HTTP push and dials port 8766, which is why this process takes 8766
and the ESP32 daemon moves to 8767.

    # terminal 1 -- owns the extension port, serves the e-ink board
    python3 -m daemon.lyrics_display_daemon serve

    # terminal 2 -- serves the SmallTV, restartable on its own
    python3 -m daemon.lyrics_display_esp8266 serve

Neither board needs reflashing: the ESP32 reads its port from the mDNS record
and the ESP8266 keeps the port it always used.
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
    _runpy.run_module("daemon.lyrics_display_esp8266", run_name="__main__", alter_sys=True)
    raise SystemExit(0)

import sys

from .lyrics.cli import main

ESP8266_DEFAULTS = ("--boards", "esp8266", "--insecure", "--relay-upstream", "--no-mdns")


def main_esp8266(argv: list[str] | None = None) -> int:
    """Run the CLI with the ESP8266 defaults prepended.

    They go in front of the user's arguments so an explicit flag still wins --
    ``--boards esp32,esp8266`` or a ``--board-port`` override behaves normally.
    """
    args = list(sys.argv[1:] if argv is None else argv)
    command = ["serve"] if args and args[0] == "serve" else []
    rest = args[1:] if command else args
    return main(command + list(ESP8266_DEFAULTS) + rest)


if __name__ == "__main__":
    raise SystemExit(main_esp8266())

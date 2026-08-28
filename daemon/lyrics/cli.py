"""Command line entry points and the asyncio serve loop."""

from __future__ import annotations

import argparse
import asyncio
import os
import re
import signal
from pathlib import Path

from .constants import BOARD_HOST, BOARD_PORT, EXTENSION_HOST, EXTENSION_PORT
from .security import DaemonIdentity, default_identity_path, load_or_create_identity
from .state import Lyrics, TrackInfo
from .store import LyricsStore, parse_lrc_with_syllables
from .render import CoreTextFrameRenderer, FallbackFrameRenderer, FrameRenderer
from .daemon import LyricsDisplayDaemon
from .discovery import DnsSdAdvertiser

def build_renderer(font_name: str) -> FrameRenderer:
    try:
        return CoreTextFrameRenderer(font_name)
    except ImportError as exc:
        print(
            f"Core Text renderer unavailable ({exc}); using geometry-only fallback "
            "(rules/progress only, no text)"
        )
        return FallbackFrameRenderer()


def default_db_path() -> str:
    return str(Path.home() / ".g4pys" / "lyrics-display.sqlite3")


def track_from_import_args(args: argparse.Namespace, file_path: Path) -> TrackInfo:
    title = args.title or file_path.stem
    return TrackInfo(
        video_id=args.video_id or file_path.stem,
        title=title,
        artist=args.artist or "",
        album=args.album or "",
        duration_sec=float(args.duration or 0),
    )


def import_lrc_command(args: argparse.Namespace) -> int:
    store = LyricsStore(Path(args.db))
    imported = 0
    for filename in args.files:
        file_path = Path(filename)
        text = file_path.read_text(encoding="utf-8")
        synced, syllables = parse_lrc_with_syllables(text)
        if synced:
            lyrics = Lyrics(synced=synced, syllables=syllables, resolved=True)
        else:
            lyrics = Lyrics(plain=[line.strip() for line in text.splitlines() if line.strip()], resolved=True)
        store.save_manual(track_from_import_args(args, file_path), lyrics)
        imported += 1
    print(f"imported {imported} manual LRC file(s)")
    return 0


def cache_command(args: argparse.Namespace) -> int:
    store = LyricsStore(Path(args.db))
    if args.cache_command == "clear":
        deleted = store.clear_cache(source=args.source, video_id=args.video_id)
        print(f"deleted {deleted} row(s) where source={args.source!r}")
        return 0
    if args.cache_command == "stats":
        rows = store.stats()
        if not rows:
            print("cache is empty")
            return 0
        for row in rows:
            print(f"{row['source']}: rows={row['rows']} negative={row['negative_rows'] or 0}")
        return 0
    raise ValueError(f"unknown cache command {args.cache_command}")


async def run(args: argparse.Namespace) -> None:
    store = LyricsStore(Path(args.db))
    # --insecure: run with no identity at all. Boards then connect over proto=1
    # with no token and frames go out unwrapped (no SEC2/HMAC). This is what the
    # ESP8266 needs -- it lacks the heap to buffer-and-verify a whole SEC2 record
    # on top of its framebuffer. Trades LAN-link authentication for ~7 KB of heap.
    if args.insecure:
        identity = None
    else:
        identity = load_or_create_identity(Path(args.identity))
        if args.board_token:
            if not re.fullmatch(r"[0-9a-fA-F]{64}", args.board_token):
                raise ValueError("--board-token must be exactly 64 hexadecimal characters")
            identity = DaemonIdentity(identity.daemon_uuid, args.board_token)
    daemon = LyricsDisplayDaemon(store, build_renderer(args.font),
                                 board_token=identity.token if identity else "",
                                 identity=identity, allow_legacy_proto1=args.allow_legacy_proto1)
    daemon_uuid = identity.daemon_uuid if identity else ""
    advertiser = DnsSdAdvertiser(
        args.mdns_instance,
        "_lyrics",
        "_tcp",
        args.board_port,
        txt=({"path": "/board", "proto": "2", "auth": "hmac-sha256", "uuid": daemon_uuid}
             if identity else
             {"path": "/board?proto=1", "proto": "1", "auth": "none"}),
    )
    legacy_advertiser = DnsSdAdvertiser(
        args.mdns_instance + " Legacy", "_lyrics", "_tcp", args.board_port,
        txt={"path": "/board?proto=1", "proto": "1", "auth": "token", "uuid": daemon_uuid},
    ) if (identity and args.allow_legacy_proto1) else None
    extension_server = await asyncio.start_server(
        daemon.handle_extension, args.extension_host, args.extension_port
    )
    board_server = await asyncio.start_server(daemon.handle_board, args.board_host, args.board_port)
    print(f"extension WebSocket: ws://{args.extension_host}:{args.extension_port}/extension")
    print(f"board WebSocket: ws://{args.board_host}:{args.board_port}/board")
    if identity:
        print(f"secure board identity: {identity.daemon_uuid}")
        print("board WebSocket auth: mutual nonce/HMAC (proto=2)")
    else:
        print("board WebSocket auth: DISABLED (--insecure): proto=1, unauthenticated")
    if not args.no_mdns:
        advertiser.start()
        if legacy_advertiser is not None:
            legacy_advertiser.start()
    # SIGTERM and SIGHUP (closed terminal) otherwise kill us outright, skipping
    # the finally below and orphaning the dns-sd child. Turn them into a normal
    # unwind so stop() runs. SIGKILL still can't be caught -- _reap_stale_dns_sd
    # covers that case on the next start.
    loop = asyncio.get_running_loop()
    stopping = loop.create_future()
    for sig in (signal.SIGTERM, signal.SIGHUP):
        try:
            loop.add_signal_handler(
                sig, lambda: stopping.done() or stopping.set_result(None))
        except (NotImplementedError, RuntimeError):
            pass
    try:
        async with extension_server, board_server:
            serving = [asyncio.ensure_future(extension_server.serve_forever()),
                       asyncio.ensure_future(board_server.serve_forever())]
            try:
                await asyncio.wait(serving + [stopping], return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in serving:
                    task.cancel()
                await asyncio.gather(*serving, return_exceptions=True)
    finally:
        advertiser.stop()
        if legacy_advertiser is not None:
            legacy_advertiser.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command")

    serve_parser = subparsers.add_parser("serve", help="run the daemon")
    for target in (parser, serve_parser):
        target.add_argument("--extension-host", default=os.environ.get("G4PYS_LYRICS_EXTENSION_HOST", EXTENSION_HOST))
        target.add_argument("--extension-port", type=int, default=int(os.environ.get("G4PYS_LYRICS_EXTENSION_PORT", str(EXTENSION_PORT))))
        target.add_argument("--board-host", default=os.environ.get("G4PYS_LYRICS_BOARD_HOST", BOARD_HOST))
        target.add_argument("--board-port", type=int, default=int(os.environ.get("G4PYS_LYRICS_BOARD_PORT", str(BOARD_PORT))))
        target.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
        target.add_argument("--font", default=os.environ.get("G4PYS_LYRICS_FONT", "Sukhumvit Set Semi Bold"))
        target.add_argument("--mdns-instance", default=os.environ.get("G4PYS_LYRICS_MDNS_INSTANCE", "g4pys Lyrics Display"))
        target.add_argument("--board-token", default=os.environ.get("G4PYS_LYRICS_BOARD_TOKEN", ""))
        target.add_argument("--identity", default=os.environ.get("G4PYS_LYRICS_IDENTITY", str(default_identity_path())))
        target.add_argument("--allow-legacy-proto1", action="store_true",
                            default=os.environ.get("G4PYS_LYRICS_ALLOW_LEGACY_PROTO1") == "1")
        target.add_argument("--insecure", action="store_true",
                            default=os.environ.get("G4PYS_LYRICS_INSECURE") == "1",
                            help="run with no board identity: boards connect unauthenticated "
                                 "(proto=1, unwrapped frames, no pairing token). Needed for the "
                                 "ESP8266, which lacks the heap for the SEC2 record layer.")
        target.add_argument("--no-mdns", action="store_true", default=os.environ.get("G4PYS_LYRICS_NO_MDNS") == "1")

    import_parser = subparsers.add_parser("import-lrc", help="import one or more .lrc files as manual lyrics")
    import_parser.add_argument("files", nargs="+")
    import_parser.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
    import_parser.add_argument("--video-id", default="")
    import_parser.add_argument("--title", default="")
    import_parser.add_argument("--artist", default="")
    import_parser.add_argument("--album", default="")
    import_parser.add_argument("--duration", type=float, default=0.0)

    cache_parser = subparsers.add_parser("cache", help="inspect or clear cached lyrics")
    cache_parser.add_argument("--db", default=os.environ.get("G4PYS_LYRICS_DB", default_db_path()))
    cache_subparsers = cache_parser.add_subparsers(dest="cache_command", required=True)
    clear_parser = cache_subparsers.add_parser("clear", help="delete cache rows by source")
    clear_parser.add_argument("--source", default="lrclib", choices=("lrclib", "manual"))
    clear_parser.add_argument("--video-id", default="")
    cache_subparsers.add_parser("stats", help="show cache row counts")

    pairing_parser = subparsers.add_parser("pairing-token", help="print the token to enter on a board")
    pairing_parser.add_argument("--identity", default=os.environ.get("G4PYS_LYRICS_IDENTITY", str(default_identity_path())))

    args = parser.parse_args()
    if args.command == "import-lrc":
        return import_lrc_command(args)
    if args.command == "cache":
        return cache_command(args)
    if args.command == "pairing-token":
        identity = load_or_create_identity(Path(args.identity))
        print(f"daemon UUID: {identity.daemon_uuid}")
        print(f"pairing token: {identity.token}")
        return 0
    if args.command == "serve":
        asyncio.run(run(args))
        return 0
    # Backward-compatible default: options without a subcommand still run the daemon.
    asyncio.run(run(args))
    return 0


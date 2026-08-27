"""Playback-state relay between daemon processes.

The browser extension speaks to exactly one address (``ws://127.0.0.1:8765``)
and the protocol is one-way, extension -> daemon. Once the ESP8266 runs as its
own process it therefore has no state source, so the daemon that owns the
extension port re-broadcasts every extension message, verbatim, to any
downstream daemon subscribed at ``/relay``.

Relaying the *raw* messages rather than resolved state is deliberate: the
downstream daemon runs the same ``handle_extension_message``, so ``event``/
``ended`` bookkeeping and ``set-theme`` replay identically with no second
protocol to keep in sync. The one synthetic message is the snapshot sent on
subscribe, so a downstream that connects mid-song does not wait for the next
track change.

Fan-out is bounded and isolating: each subscriber has the same capped outbound
queue a board gets, and a subscriber that stops reading is dropped rather than
allowed to block the upstream render path -- which is the whole point of running
the boards in separate processes.
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import secrets
import struct
from typing import Any, Callable

from .wsproto import WebSocketConnection

RELAY_PATH = "/relay"
RELAY_RECONNECT_MIN_SECONDS = 1.0
RELAY_RECONNECT_MAX_SECONDS = 30.0
RELAY_CONNECT_TIMEOUT_SECONDS = 5.0


class RelayHub:
    """The upstream side: keeps subscribers and pushes messages to them."""

    def __init__(self) -> None:
        self.subscribers: set[WebSocketConnection] = set()

    def add(self, conn: WebSocketConnection) -> None:
        conn.start_sender()
        self.subscribers.add(conn)

    def discard(self, conn: WebSocketConnection) -> None:
        self.subscribers.discard(conn)

    async def broadcast(self, raw: str) -> None:
        """Push one extension message to every subscriber.

        Never raises: a subscriber whose bounded queue is full has already
        fallen several messages behind, so ``send`` closes it. Dropping it here
        keeps a stalled downstream daemon from stalling this one.
        """
        if not self.subscribers:
            return
        payload = raw.encode("utf-8")
        for conn in list(self.subscribers):
            try:
                await conn.send(payload, opcode=1)
            except (ConnectionError, OSError):
                self.discard(conn)
                conn.close()
                print("relay subscriber dropped (not keeping up)")


# ---------------------------------------------------------------------------
# Downstream client
# ---------------------------------------------------------------------------

def _mask_frame(payload: bytes, opcode: int) -> bytes:
    """Encode one client->server frame. RFC 6455 requires client masking."""
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", length))
    mask = os.urandom(4)
    header.extend(mask)
    masked = bytearray(payload)
    for i in range(length):
        masked[i] ^= mask[i % 4]
    return bytes(header) + bytes(masked)


class RelayClient:
    """The downstream side: subscribes to an upstream daemon and replays it.

    Reconnects forever with exponential backoff, and never blocks startup -- the
    board keeps rendering its idle screen until the upstream daemon appears.
    """

    def __init__(self, host: str, port: int,
                 on_message: Callable[[str], Any],
                 on_snapshot: Callable[[dict[str, Any]], Any] | None = None) -> None:
        self.host = host
        self.port = port
        self.on_message = on_message
        self.on_snapshot = on_snapshot
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def run(self) -> None:
        delay = RELAY_RECONNECT_MIN_SECONDS
        while True:
            try:
                await self._session()
                delay = RELAY_RECONNECT_MIN_SECONDS
            except asyncio.CancelledError:
                raise
            except (OSError, ConnectionError, ValueError, asyncio.TimeoutError,
                    asyncio.IncompleteReadError, json.JSONDecodeError) as exc:
                print(f"relay upstream {self.host}:{self.port} unavailable ({exc}); "
                      f"retrying in {delay:.0f}s")
            finally:
                self._close()
            await asyncio.sleep(delay)
            delay = min(delay * 2, RELAY_RECONNECT_MAX_SECONDS)

    async def _session(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), RELAY_CONNECT_TIMEOUT_SECONDS
        )
        await self._handshake()
        print(f"relay connected to upstream {self.host}:{self.port}")
        while True:
            message = await self._recv()
            if message is None:
                print("relay upstream closed the connection")
                return
            opcode, payload = message
            if opcode != 1:
                continue
            raw = payload.decode("utf-8", "replace")
            if self.on_snapshot is not None:
                try:
                    parsed = json.loads(raw)
                except json.JSONDecodeError:
                    parsed = None
                if isinstance(parsed, dict) and parsed.get("type") == "relay-snapshot":
                    await self.on_snapshot(parsed.get("payload") or {})
                    continue
            await self.on_message(raw)

    async def _handshake(self) -> None:
        assert self._reader is not None and self._writer is not None
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        request = (
            f"GET {RELAY_PATH} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self._writer.write(request.encode("ascii"))
        await self._writer.drain()
        status = await self._reader.readline()
        if b"101" not in status:
            raise ValueError(f"relay upgrade refused: {status!r}")
        while True:
            line = await self._reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break

    async def _recv(self) -> tuple[int, bytes] | None:
        """Read one server frame. Servers never mask; control frames are handled."""
        assert self._reader is not None
        while True:
            try:
                first = await self._reader.readexactly(2)
            except asyncio.IncompleteReadError:
                return None
            fin = bool(first[0] & 0x80)
            opcode = first[0] & 0x0F
            if first[1] & 0x80:
                raise ValueError("masked server websocket frame")
            length = first[1] & 0x7F
            if length == 126:
                length = struct.unpack("!H", await self._reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", await self._reader.readexactly(8))[0]
            payload = await self._reader.readexactly(length) if length else b""
            if opcode == 8:
                return None
            if opcode == 9:
                await self._send(payload, opcode=10)
                continue
            if opcode == 10:
                continue
            if not fin:
                raise ValueError("fragmented relay message")
            return opcode, payload

    async def _send(self, payload: bytes, opcode: int) -> None:
        if self._writer is None:
            return
        self._writer.write(_mask_frame(payload, opcode))
        await self._writer.drain()

    def _close(self) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
            except OSError:
                pass
        self._reader = None
        self._writer = None

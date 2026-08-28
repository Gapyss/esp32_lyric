"""Minimal WebSocket server connection with the optional SEC2 record layer."""

from __future__ import annotations

import asyncio
import base64
import hmac
import hashlib
import json
import struct
from typing import Any, Callable

from .constants import BOARD_QUEUE_DEPTH, HEARTBEAT_INTERVAL_SECONDS, HEARTBEAT_TIMEOUT_SECONDS, SEC2_BINARY, SEC2_HEADER_STRUCT, SEC2_MAGIC, SEC2_TAG_BYTES, SEC2_TEXT, SEC2_VERSION
from .security import monotonic_ms
from .profiles import DEFAULT_PROFILE, RenderProfile

class WebSocketConnection:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.reader = reader
        self.writer = writer
        self.write_lock = asyncio.Lock()
        self.board_frame_base: bytes | None = None
        self.board_color_key: tuple[int, int, int, int, bytes] | None = None
        self.profile: RenderProfile = DEFAULT_PROFILE
        self.supports_rgb565 = False
        self.tx_key: bytes | None = None
        self.rx_key: bytes | None = None
        self.tx_sequence = 0
        self.rx_sequence = 0
        self.outbound: asyncio.Queue[tuple[bytes, int]] | None = None
        self.sender_task: asyncio.Task[None] | None = None
        self.backpressure_events = 0
        self.pong_event = asyncio.Event()
        self.closed = False

    async def handshake(self, accept: Callable[[str, dict[str, str]], bool] | None = None) -> str:
        raw = await self.reader.readuntil(b"\r\n\r\n")
        head = raw.decode("iso-8859-1")
        lines = head.split("\r\n")
        request = lines[0].split()
        path = request[1] if len(request) > 1 else "/"
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.lower()] = value.strip()
        key = headers.get("sec-websocket-key")
        if not key:
            raise ValueError("missing Sec-WebSocket-Key")
        if accept is not None and not accept(path, headers):
            self.writer.write(b"HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            await self.writer.drain()
            raise ValueError("websocket handshake rejected")
        accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        self.writer.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode("ascii")
        )
        await self.writer.drain()
        return path

    async def recv(self) -> tuple[int, bytes] | None:
        fragment_opcode: int | None = None
        fragments: list[bytes] = []
        while True:
            try:
                fin, opcode, payload = await self._read_frame()
            except asyncio.IncompleteReadError:
                return None
            if opcode == 8:
                await self.send(payload, opcode=8)
                return None
            if opcode == 9:
                await self.send(payload, opcode=10)
                continue
            if opcode == 10:
                self.pong_event.set()
                continue
            if opcode in (1, 2):
                if fragment_opcode is not None:
                    raise ValueError("new data frame before fragmented message completed")
                if fin:
                    return opcode, payload
                fragment_opcode = opcode
                fragments = [payload]
                continue
            if opcode == 0:
                if fragment_opcode is None:
                    raise ValueError("unexpected continuation frame")
                fragments.append(payload)
                if fin:
                    complete_opcode = fragment_opcode
                    complete_payload = b"".join(fragments)
                    fragment_opcode = None
                    fragments = []
                    return complete_opcode, complete_payload
                continue
            raise ValueError(f"unsupported websocket opcode {opcode}")

    async def _read_frame(self) -> tuple[bool, int, bytes]:
        first = await self.reader.readexactly(2)
        fin = bool(first[0] & 0x80)
        opcode = first[0] & 0x0F
        masked = bool(first[1] & 0x80)
        length = first[1] & 0x7F
        if not masked:
            raise ValueError("unmasked client websocket frame")
        if length == 126:
            length = struct.unpack("!H", await self.reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", await self.reader.readexactly(8))[0]
        if opcode in (8, 9, 10) and (not fin or length > 125):
            raise ValueError("invalid websocket control frame")
        mask = await self.reader.readexactly(4)
        payload = bytearray(await self.reader.readexactly(length))
        for i in range(length):
            payload[i] ^= mask[i % 4]
        return fin, opcode, bytes(payload)

    async def send_text(self, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if self.tx_key is not None:
            await self.send(self._protect(raw, SEC2_TEXT), opcode=2)
        else:
            await self.send(raw, opcode=1)

    async def send_binary(self, payload: bytes) -> None:
        await self.send(self._protect(payload, SEC2_BINARY) if self.tx_key is not None else payload, opcode=2)

    def enable_security(self, tx_key: bytes, rx_key: bytes) -> None:
        self.tx_key = tx_key
        self.rx_key = rx_key
        self.tx_sequence = 0
        self.rx_sequence = 0

    def _protect(self, payload: bytes, content_type: int) -> bytes:
        if self.tx_key is None:
            raise ValueError("secure session is not initialized")
        self.tx_sequence += 1
        header = SEC2_HEADER_STRUCT.pack(SEC2_MAGIC, SEC2_VERSION, content_type, 0, self.tx_sequence, len(payload))
        return header + payload + hmac.new(self.tx_key, header + payload, hashlib.sha256).digest()

    def _unprotect(self, record: bytes) -> tuple[int, bytes]:
        if self.rx_key is None or len(record) < SEC2_HEADER_STRUCT.size + SEC2_TAG_BYTES:
            raise ValueError("invalid SEC2 record")
        header = record[: SEC2_HEADER_STRUCT.size]
        magic, version, content_type, reserved, sequence, payload_len = SEC2_HEADER_STRUCT.unpack(header)
        expected_len = SEC2_HEADER_STRUCT.size + payload_len + SEC2_TAG_BYTES
        if magic != SEC2_MAGIC or version != SEC2_VERSION or reserved != 0 or expected_len != len(record):
            raise ValueError("invalid SEC2 header")
        if sequence != self.rx_sequence + 1:
            raise ValueError("SEC2 replay or sequence gap")
        payload = record[SEC2_HEADER_STRUCT.size : -SEC2_TAG_BYTES]
        tag = record[-SEC2_TAG_BYTES:]
        expected = hmac.new(self.rx_key, header + payload, hashlib.sha256).digest()
        if not hmac.compare_digest(tag, expected):
            raise ValueError("SEC2 authentication failed")
        if content_type not in (SEC2_TEXT, SEC2_BINARY):
            raise ValueError("invalid SEC2 content type")
        self.rx_sequence = sequence
        return content_type, payload

    async def recv_application(self) -> tuple[int, bytes] | None:
        message = await self.recv()
        if message is None:
            return None
        opcode, payload = message
        if self.rx_key is None:
            return message
        if opcode != 2:
            raise ValueError("secure application data must use binary WebSocket frames")
        content_type, clear = self._unprotect(payload)
        return (1 if content_type == SEC2_TEXT else 2), clear

    def start_sender(self) -> None:
        if self.outbound is not None:
            return
        self.outbound = asyncio.Queue(maxsize=BOARD_QUEUE_DEPTH)
        self.sender_task = asyncio.create_task(self._sender_loop())

    async def _sender_loop(self) -> None:
        assert self.outbound is not None
        try:
            while True:
                payload, opcode = await self.outbound.get()
                async with self.write_lock:
                    self._write_frame(payload, opcode)
                    await self.writer.drain()
                self.backpressure_events = 0
        except (asyncio.CancelledError, OSError, ConnectionError):
            pass
        finally:
            self.closed = True
            self.writer.close()

    async def send(self, payload: bytes, opcode: int) -> None:
        if self.closed:
            raise ConnectionError("board connection is closed")
        if self.outbound is not None:
            try:
                self.outbound.put_nowait((payload, opcode))
                return
            except asyncio.QueueFull:
                self.backpressure_events += 1
                # A full bounded queue means this board has already remained
                # behind for several complete messages. SEC2 sequences cannot
                # safely skip a dropped record, so isolate it immediately.
                self.close()
                raise ConnectionError("board outbound queue is full")
        async with self.write_lock:
            self._write_frame(payload, opcode)
            await self.writer.drain()

    def _write_frame(self, payload: bytes, opcode: int) -> None:
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length < 65536:
            header.extend((126, *struct.pack("!H", length)))
        else:
            header.extend((127, *struct.pack("!Q", length)))
        self.writer.write(bytes(header) + payload)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.sender_task is not None:
            self.sender_task.cancel()
        self.writer.close()

    async def heartbeat(self) -> None:
        while not self.closed:
            await asyncio.sleep(HEARTBEAT_INTERVAL_SECONDS)
            self.pong_event.clear()
            await self.send(str(monotonic_ms()).encode("ascii"), opcode=9)
            try:
                await asyncio.wait_for(self.pong_event.wait(), HEARTBEAT_TIMEOUT_SECONDS)
            except asyncio.TimeoutError:
                self.close()
                raise ConnectionError("board heartbeat timed out")

"""mDNS/DNS-SD advertisement of the board endpoint as _lyrics._tcp."""

from __future__ import annotations

import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time

from .constants import MDNS_GROUP, MDNS_PORT

def mdns_encode_name(name: str) -> bytes:
    out = bytearray()
    for label in name.rstrip(".").split("."):
        raw = label.encode("utf-8")
        if len(raw) > 63:
            raw = raw[:63]
        out.append(len(raw))
        out.extend(raw)
    out.append(0)
    return bytes(out)


def mdns_read_name(packet: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    jumped = False
    next_offset = offset
    seen = 0
    while offset < len(packet) and seen < 32:
        seen += 1
        length = packet[offset]
        if length == 0:
            offset += 1
            if not jumped:
                next_offset = offset
            break
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                break
            pointer = ((length & 0x3F) << 8) | packet[offset + 1]
            if not jumped:
                next_offset = offset + 2
            offset = pointer
            jumped = True
            continue
        offset += 1
        labels.append(packet[offset : offset + length].decode("utf-8", "replace"))
        offset += length
        if not jumped:
            next_offset = offset
    return ".".join(labels).lower(), next_offset


def mdns_query_names(packet: bytes) -> list[str]:
    if len(packet) < 12:
        return []
    qdcount = struct.unpack("!H", packet[4:6])[0]
    offset = 12
    names: list[str] = []
    for _idx in range(qdcount):
        name, offset = mdns_read_name(packet, offset)
        if offset + 4 > len(packet):
            break
        offset += 4
        names.append(name.rstrip("."))
    return names


def mdns_rr(name: str, rrtype: int, rrclass: int, ttl: int, rdata: bytes) -> bytes:
    return mdns_encode_name(name) + struct.pack("!HHIH", rrtype, rrclass, ttl, len(rdata)) + rdata


def local_ipv4() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return str(sock.getsockname()[0])
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"


class DnsSdAdvertiser:
    def __init__(self, instance: str, service: str, proto: str, port: int, txt: dict[str, str] | None = None) -> None:
        self.instance = instance
        self.service = service
        self.proto = proto
        self.port = port
        self.txt = txt or {}
        self.proc: subprocess.Popen[bytes] | None = None
        self.sock: socket.socket | None = None
        self.stop_event = threading.Event()
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        # On macOS, Bonjour owns interface scoping and publishes the correct A
        # records per interface (including when VPNs come and go). Prefer it to
        # synthesizing one host address ourselves.
        if shutil.which("dns-sd") is not None:
            self._start_dns_sd()
            return
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            self.sock.bind(("", MDNS_PORT))
            mreq = socket.inet_aton(MDNS_GROUP) + socket.inet_aton("0.0.0.0")
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            self.sock.settimeout(0.5)
        except OSError as exc:
            print(f"native mDNS unavailable ({exc}); trying dns-sd fallback")
            self.sock = None
            self._start_dns_sd()
            return
        self.thread = threading.Thread(target=self._serve, name="lyrics-mdns", daemon=True)
        self.thread.start()
        self._send_announcement()
        print(f"mDNS advertisement: {self.instance}.{self.service}.{self.proto}.local:{self.port}")

    def _reap_stale_dns_sd(self) -> None:
        # A daemon that exits without unwinding never reaches stop(), so its
        # dns-sd -R child survives reparented to init and keeps advertising a
        # stale TXT record. Because every run registers the same instance name,
        # Bonjour lets the oldest holder own it -- boards then read the dead
        # daemon's TXT (e.g. proto=1 from an old --insecure run), reject it, and
        # sit in RETRY_WAIT forever. Nothing we do at shutdown can cover a
        # SIGKILL, so clear the name at startup instead.
        needle = f"-R {self.instance} {self.service}.{self.proto} "
        try:
            listing = subprocess.run(["ps", "-axo", "pid=,command="],
                                     capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            return
        for line in listing.splitlines():
            pid_text, _, command = line.strip().partition(" ")
            if "dns-sd" not in command or needle not in command:
                continue
            try:
                os.kill(int(pid_text), signal.SIGTERM)
            except (ValueError, OSError):
                continue
            print(f"reaped stale dns-sd registration (pid {pid_text})")

    def _start_dns_sd(self) -> None:
        dns_sd = shutil.which("dns-sd")
        if dns_sd is None:
            print("dns-sd unavailable; _lyrics._tcp mDNS advertisement disabled")
            return
        self._reap_stale_dns_sd()
        try:
            self.proc = subprocess.Popen(
                [dns_sd, "-R", self.instance, f"{self.service}.{self.proto}", "local", str(self.port),
                 *[f"{key}={value}" for key, value in self.txt.items()]],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError as exc:
            print(f"dns-sd failed to start ({exc}); _lyrics._tcp mDNS advertisement disabled")
            return
        print(f"mDNS advertisement: {self.instance}.{self.service}.{self.proto}.local:{self.port}")

    @property
    def service_name(self) -> str:
        return f"{self.service}.{self.proto}.local"

    @property
    def instance_name(self) -> str:
        return f"{self.instance}.{self.service}.{self.proto}.local"

    @property
    def host_name(self) -> str:
        safe = re.sub(r"[^a-zA-Z0-9-]+", "-", socket.gethostname()).strip("-") or "g4pys-lyrics"
        return f"{safe}.local"

    def _response_packet(self) -> bytes:
        txt_items = [f"{key}={value}".encode("utf-8")[:255] for key, value in self.txt.items()]
        txt_rdata = b"".join(bytes([len(item)]) + item for item in txt_items) or b"\x00"
        answers = [
            mdns_rr(self.service_name, 12, 1, 120, mdns_encode_name(self.instance_name)),
            mdns_rr(self.instance_name, 33, 0x8001, 120, struct.pack("!HHH", 0, 0, self.port) + mdns_encode_name(self.host_name)),
            mdns_rr(self.instance_name, 16, 0x8001, 120, txt_rdata),
            mdns_rr(self.host_name, 1, 0x8001, 120, socket.inet_aton(local_ipv4())),
        ]
        return struct.pack("!HHHHHH", 0, 0x8400, 0, len(answers), 0, 0) + b"".join(answers)

    def _send_announcement(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.sendto(self._response_packet(), (MDNS_GROUP, MDNS_PORT))
        except OSError:
            pass

    def _serve(self) -> None:
        assert self.sock is not None
        last_announce = 0.0
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - last_announce > 30:
                self._send_announcement()
                last_announce = now
            try:
                packet, _addr = self.sock.recvfrom(2048)
            except TimeoutError:
                continue
            except OSError:
                break
            names = mdns_query_names(packet)
            if any(name in {self.service_name, self.instance_name, self.host_name} for name in names):
                self._send_announcement()

    def stop(self) -> None:
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.thread is not None:
            self.thread.join(timeout=1)
            self.thread = None
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=2)
        self.proc = None

"""Daemon identity, pairing token and the proto=2 session key derivation."""

from __future__ import annotations

import hmac
import hashlib
import json
import os
import re
import secrets
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

@dataclass(frozen=True)
class DaemonIdentity:
    daemon_uuid: str
    token: str


def default_identity_path() -> Path:
    return Path.home() / ".g4pys" / "lyrics-identity.json"


def load_or_create_identity(path: Path | None = None) -> DaemonIdentity:
    """Load the long-lived daemon UUID and pairing secret, creating them safely."""
    path = path or default_identity_path()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        os.chmod(path.parent, 0o700)
    except OSError:
        pass
    if path.exists():
        raw = json.loads(path.read_text(encoding="utf-8"))
        daemon_uuid = str(uuid.UUID(str(raw["daemon_uuid"])))
        token = str(raw["token"])
        if not re.fullmatch(r"[0-9a-f]{64}", token):
            raise ValueError(f"invalid token in {path}")
        os.chmod(path, 0o600)
        return DaemonIdentity(daemon_uuid, token)

    identity = DaemonIdentity(str(uuid.uuid4()), secrets.token_hex(32))
    payload = (json.dumps({"version": 1, "daemon_uuid": identity.daemon_uuid, "token": identity.token}, indent=2) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    return identity


def _auth_transcript(role: str, daemon_uuid: str, server_nonce: bytes, client_nonce: bytes) -> bytes:
    return b"lyrics-v2/" + role.encode("ascii") + b"\0" + daemon_uuid.encode("ascii") + b"\0" + server_nonce + client_nonce


def derive_session_keys(token: str, server_nonce: bytes, client_nonce: bytes) -> tuple[bytes, bytes]:
    """RFC 5869 HKDF-SHA256; returns (client->server, server->client)."""
    salt = server_nonce + client_nonce
    prk = hmac.new(salt, token.encode("ascii"), hashlib.sha256).digest()
    output = b""
    previous = b""
    counter = 1
    while len(output) < 64:
        previous = hmac.new(prk, previous + b"lyrics-v2/session" + bytes([counter]), hashlib.sha256).digest()
        output += previous
        counter += 1
    return output[:32], output[32:64]


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))

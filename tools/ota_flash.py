#!/usr/bin/env python3
"""Flash the ESP32-S3 board over WiFi.

Uploads a built firmware image to the board's token-gated POST /ota endpoint,
which writes it to the inactive OTA slot and reboots into it. Uses only the
standard library, so it needs no virtualenv.

The token is generated on the board's first boot and printed to the serial log
on every boot ("OTA upload token: ..."). Pass it with --token or put it in
G4PYS_OTA_TOKEN.

    tools/ota_flash.py --host g4pys.local
    tools/ota_flash.py --host 192.168.1.42 --bin firmware/build/g4pys_company_rlcd.bin
    tools/ota_flash.py --host g4pys.local --status
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_BIN = Path(__file__).resolve().parent.parent / "firmware" / "build" / "g4pys_company_rlcd.bin"
# Writing ~1.3 MB to flash while the board keeps rendering takes a while; the
# response only comes back once the whole image is validated.
UPLOAD_TIMEOUT_SEC = 180
STATUS_TIMEOUT_SEC = 10


def base_url(host: str) -> str:
    if host.startswith(("http://", "https://")):
        return host.rstrip("/")
    return f"http://{host}".rstrip("/")


def fetch_status(host: str) -> dict:
    with urllib.request.urlopen(f"{base_url(host)}/ota", timeout=STATUS_TIMEOUT_SEC) as response:
        return json.loads(response.read().decode("utf-8"))


def _post(host: str, token: str, payload: bytes, timeout: int) -> dict:
    request = urllib.request.Request(
        f"{base_url(host)}/ota",
        data=payload,
        method="POST",
        headers={
            "X-OTA-Token": token,
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(payload)),
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def check_token(host: str, token: str) -> dict:
    """Empty POST the board answers as an auth probe.

    Worth the extra round trip: a bad token on the real upload is answered
    while a megabyte is still in flight, so the client sees a broken pipe
    instead of the 401.
    """
    return _post(host, token, b"", STATUS_TIMEOUT_SEC)


def upload(host: str, token: str, image: bytes) -> dict:
    return _post(host, token, image, UPLOAD_TIMEOUT_SEC)


def wait_for_reboot(host: str, expected_partition: str, timeout_sec: int = 90) -> dict | None:
    """Poll /ota until the board answers again from the partition it just wrote."""
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        time.sleep(3)
        try:
            status = fetch_status(host)
        except (urllib.error.URLError, OSError, json.JSONDecodeError):
            continue
        if status.get("running") == expected_partition:
            return status
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="board hostname or IP, e.g. g4pys.local")
    parser.add_argument("--bin", type=Path, default=DEFAULT_BIN, help=f"image to upload (default: {DEFAULT_BIN})")
    parser.add_argument("--token", default=os.environ.get("G4PYS_OTA_TOKEN"), help="OTA token, or set G4PYS_OTA_TOKEN")
    parser.add_argument("--status", action="store_true", help="print board OTA status and exit")
    parser.add_argument("--no-wait", action="store_true", help="do not wait for the board to come back up")
    args = parser.parse_args()

    if args.status:
        try:
            print(json.dumps(fetch_status(args.host), indent=2))
        except (urllib.error.URLError, OSError) as exc:
            print(f"error: could not reach {args.host}: {exc}", file=sys.stderr)
            return 1
        return 0

    if not args.token:
        print("error: no token. Pass --token or set G4PYS_OTA_TOKEN.", file=sys.stderr)
        print("       The board prints it at boot: 'OTA upload token: ...'", file=sys.stderr)
        return 2
    if not args.bin.is_file():
        print(f"error: {args.bin} not found. Build first: idf.py -C firmware build", file=sys.stderr)
        return 2

    image = args.bin.read_bytes()

    try:
        before = fetch_status(args.host)
    except (urllib.error.URLError, OSError) as exc:
        print(f"error: could not reach {args.host}: {exc}", file=sys.stderr)
        return 1

    target = before.get("next", "?")
    capacity = before.get("capacity", 0)
    print(f"board is running '{before.get('running')}' (built {before.get('built')})")
    if capacity and len(image) > capacity:
        print(f"error: image is larger than the {capacity} byte OTA slot", file=sys.stderr)
        return 1

    try:
        check_token(args.host, args.token)
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            print("error: board rejected the token (401).", file=sys.stderr)
            print("       It is printed at every boot: 'OTA upload token: ...'", file=sys.stderr)
            return 1
        print(f"error: board returned HTTP {exc.code} to the auth check", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as exc:
        print(f"error: auth check failed: {exc}", file=sys.stderr)
        return 1

    print(f"uploading {args.bin.name}, {len(image) / 1024:.0f} KiB -> '{target}'")
    started = time.time()
    try:
        result = upload(args.host, args.token, image)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace").strip()
        if exc.code == 401:
            print("error: board rejected the token (401). Check the boot log.", file=sys.stderr)
        else:
            print(f"error: board returned HTTP {exc.code}: {detail}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as exc:
        print(f"error: upload failed: {exc}", file=sys.stderr)
        print("       The board discards a partial image, so it is still running the old one.", file=sys.stderr)
        return 1

    print(f"accepted in {time.time() - started:.0f}s; board is rebooting into '{result.get('booting')}'")
    if args.no_wait:
        return 0

    after = wait_for_reboot(args.host, target)
    if after is None:
        print("warning: board did not answer after the reboot. Check it over serial.", file=sys.stderr)
        return 1
    print(f"back up, running '{after.get('running')}' (built {after.get('built')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

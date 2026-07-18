#!/usr/bin/env python3
"""Render now-playing text to Core Text bitmaps and forward to the ESP32."""

from __future__ import annotations

import argparse
import http.server
import math
import os
import struct
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass

try:
    import CoreText
    import Quartz
    from Foundation import NSAttributedString
except ImportError as exc:  # pragma: no cover - only exercised on an unprovisioned Mac.
    raise SystemExit(
        "PyObjC Quartz/CoreText bindings are required. Install pyobjc-framework-Quartz."
    ) from exc


SLOTS = ("title", "artist", "lyric", "lyric2", "lyric3")
NUMERIC_FIELDS = ("pos", "dur", "paused", "lt", "lt2")


@dataclass(frozen=True)
class SlotStyle:
    font_size: float
    height: int


STYLES = {
    "title": SlotStyle(24.0, 32),
    "artist": SlotStyle(18.0, 24),
    "lyric": SlotStyle(28.0, 36),
    "lyric2": SlotStyle(21.0, 28),
    "lyric3": SlotStyle(21.0, 28),
}


class CoreTextRenderer:
    def __init__(self, font_name: str) -> None:
        self.font_name = font_name
        self.color_space = Quartz.CGColorSpaceCreateDeviceGray()

    def render_slot(self, text: str, style: SlotStyle) -> bytes:
        if not text:
            return struct.pack("<HHI", 0, 0, 0)

        font = CoreText.CTFontCreateWithName(self.font_name, style.font_size, None)
        attrs = {
            CoreText.kCTFontAttributeName: font,
            CoreText.kCTForegroundColorFromContextAttributeName: True,
        }
        attributed = NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        line = CoreText.CTLineCreateWithAttributedString(attributed)

        bounds = CoreText.CTLineGetTypographicBounds(line, None, None, None)
        advance = bounds[0] if isinstance(bounds, tuple) else bounds

        # Measure the actual shaped ink (base glyphs + stacked Thai upper/lower
        # marks) so we can position it. Bounds are relative to the pen at (0, 0)
        # in the context's (bottom-left origin) user space. Deriving the pen
        # position from these measured bounds — rather than from font
        # ascent/descent — is what keeps Thai vowel/tone stacks from being
        # clipped off the top of the box.
        pad = 3
        probe = Quartz.CGBitmapContextCreate(
            None, 1, 1, 8, 1, self.color_space, Quartz.kCGImageAlphaNone
        )
        Quartz.CGContextSetTextMatrix(probe, Quartz.CGAffineTransformIdentity)
        ink = CoreText.CTLineGetImageBounds(line, probe)
        ink_x, ink_y = ink.origin.x, ink.origin.y
        ink_w, ink_h = ink.size.width, ink.size.height

        if not (ink_w > 0 and ink_h > 0):
            # Whitespace-only or unmeasurable: fall back to font metrics.
            ascent = CoreText.CTFontGetAscent(font)
            descent = CoreText.CTFontGetDescent(font)
            ink_x, ink_w = 0.0, advance
            ink_h = ascent + descent
            ink_y = -descent

        width = max(1, int(math.ceil(max(advance, ink_x + ink_w))) + 2 * pad)
        height = style.height

        ctx = Quartz.CGBitmapContextCreate(
            None,
            width,
            height,
            8,
            0,  # let CoreGraphics choose (and align) bytesPerRow
            self.color_space,
            Quartz.kCGImageAlphaNone,
        )
        Quartz.CGContextSetGrayFillColor(ctx, 0.0, 1.0)
        Quartz.CGContextFillRect(ctx, Quartz.CGRectMake(0, 0, width, height))
        Quartz.CGContextSetGrayFillColor(ctx, 1.0, 1.0)
        Quartz.CGContextSetShouldAntialias(ctx, True)
        Quartz.CGContextSetTextMatrix(ctx, Quartz.CGAffineTransformIdentity)

        # Place the pen so the measured ink box is left-padded and vertically
        # centered within the fixed-height slot.
        pen_x = pad - ink_x
        pen_y = (height - ink_h) / 2.0 - ink_y
        Quartz.CGContextSetTextPosition(ctx, pen_x, pen_y)
        CoreText.CTLineDraw(line, ctx)

        image = Quartz.CGBitmapContextCreateImage(ctx)
        provider = Quartz.CGImageGetDataProvider(image)
        pixels = bytes(Quartz.CGDataProviderCopyData(provider))
        # CoreGraphics may row-align the backing store, so trust the image's
        # reported stride rather than assuming bytesPerRow == width.
        actual_stride = Quartz.CGImageGetBytesPerRow(image)
        packed = self._pack_1bpp(pixels, width, height, actual_stride)
        return struct.pack("<HHI", width, height, len(packed)) + packed

    @staticmethod
    def _pack_1bpp(pixels: bytes, width: int, height: int, stride: int) -> bytes:
        row_bytes = (width + 7) // 8
        out = bytearray(row_bytes * height)
        for y in range(height):
            src_row = y * stride
            dst_row = y * row_bytes
            for x in range(width):
                if pixels[src_row + x] > 32:
                    out[dst_row + (x // 8)] |= 1 << (x & 7)
        return bytes(out)


def first_value(query: dict[str, list[str]], key: str, default: str = "") -> str:
    values = query.get(key)
    if not values:
        return default
    return values[0]


def make_forward_url(device_url: str, query: dict[str, list[str]]) -> str:
    params = {field: first_value(query, field, "-1" if field.startswith("lt") else "0") for field in NUMERIC_FIELDS}
    return urllib.parse.urljoin(device_url.rstrip("/") + "/", "nowplaying") + "?" + urllib.parse.urlencode(params)


class WrapperHandler(http.server.BaseHTTPRequestHandler):
    renderer: CoreTextRenderer
    device_url: str
    timeout_seconds: float

    def do_GET(self) -> None:  # noqa: N802
        self._handle_nowplaying()

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length > 0:
            self.rfile.read(length)
        self._handle_nowplaying()

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} {self.client_address[0]} {fmt % args}", file=sys.stderr)

    def _handle_nowplaying(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/nowplaying":
            self.send_error(404, "not found")
            return

        query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        body = b"".join(self.renderer.render_slot(first_value(query, slot), STYLES[slot]) for slot in SLOTS)
        forward_url = make_forward_url(self.device_url, query)
        req = urllib.request.Request(
            forward_url,
            data=body,
            method="POST",
            headers={"Content-Type": "application/octet-stream"},
        )

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_seconds) as resp:
                status = resp.status
                resp.read()
            print(f"forward ok status={status} bytes={len(body)} url={forward_url}", file=sys.stderr)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
        except Exception as exc:  # pragma: no cover - depends on live device/network.
            print(f"forward failed bytes={len(body)} url={forward_url}: {exc}", file=sys.stderr)
            self.send_error(502, "device forward failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("G4PYS_WRAPPER_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("G4PYS_WRAPPER_PORT", "8123")))
    parser.add_argument("--device-url", default=os.environ.get("G4PYS_DEVICE_URL", "http://g4pys-company.local"))
    # Sukhumvit Set covers both Thai and Latin in one face (no cascade seam),
    # which the lyric/title/artist slots need. The exact string "Sukhumvit Set
    # Semi Bold" resolves to SukhumvitSet-SemiBold; the Semi Bold weight keeps
    # stems thick enough to survive 1-bit thresholding on the reflective LCD
    # (plain "Sukhumvit Set" would select the lighter Text weight).
    parser.add_argument("--font", default=os.environ.get("G4PYS_RENDER_FONT", "Sukhumvit Set Semi Bold"))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("G4PYS_FORWARD_TIMEOUT", "2.0")))
    args = parser.parse_args()

    WrapperHandler.renderer = CoreTextRenderer(args.font)
    WrapperHandler.device_url = args.device_url
    WrapperHandler.timeout_seconds = args.timeout

    server = http.server.ThreadingHTTPServer((args.host, args.port), WrapperHandler)
    print(f"g4pys.company render wrapper listening on http://{args.host}:{args.port}", file=sys.stderr)
    print(f"forwarding to {args.device_url}", file=sys.stderr)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

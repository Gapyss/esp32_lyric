"""Frame renderers: Core Text on macOS, geometry-only fallback elsewhere."""

from __future__ import annotations

import math
from typing import Any

from .constants import BRAND_FONT_NAMES, DISPLAY_HEIGHT, DISPLAY_WIDTH, KARAOKE_STROKE_PCT, MONO_FONT_NAME, TRACKING_MEGA, TRACKING_WIDE
from .security import clamp
from .state import AppState, CoverArt
from .profiles import BOARDS, LyricLayout, ProgressGeom, RenderProfile, cover_placement
from .imaging import draw_rect, fill_rect, fit_lyric_layout, floyd_steinberg_1bit, format_time, pack_1bpp

class FrameRenderer:
    def render(self, state: AppState, profile: RenderProfile | None = None) -> bytes:
        raise NotImplementedError


class CoreTextFrameRenderer(FrameRenderer):
    def __init__(self, font_name: str = "Tahoma") -> None:
        import CoreText  # type: ignore
        import Quartz  # type: ignore
        from Foundation import NSAttributedString, NSString  # type: ignore

        self.CoreText = CoreText
        self.Quartz = Quartz
        self.NSAttributedString = NSAttributedString
        self.NSString = NSString
        self.font_name = font_name
        self.color_space = Quartz.CGColorSpaceCreateDeviceGray()
        self.rgb_color_space = Quartz.CGColorSpaceCreateDeviceRGB()
        self.brand_fonts = self._load_brand_fonts()
        # Height of the frame currently being rendered; the Core Text primitives
        # need it to flip top-origin layout coords into Quartz's bottom-origin
        # space. render() sets it per call (rendering is single-threaded).
        self._h = DISPLAY_HEIGHT

    def _load_brand_fonts(self) -> dict[str, str]:
        """Resolve the Sukhumvit Set weight used for each Tend type role.

        Sukhumvit Set is a macOS system family, so there is nothing to register.
        CTFontCreateWithName silently substitutes a default face when a name
        does not resolve, so we confirm each weight actually maps to Sukhumvit
        (its PostScript names start with "SukhumvitSet") and otherwise fall back
        to self.font_name for that role.
        """
        ct = self.CoreText
        resolved: dict[str, str] = {}
        for role, name in BRAND_FONT_NAMES.items():
            font = ct.CTFontCreateWithName(name, 24.0, None)
            ps_name = str(ct.CTFontCopyPostScriptName(font))
            if ps_name.startswith("SukhumvitSet"):
                resolved[role] = name
            else:
                print(f"font {name!r} unavailable (resolved to {ps_name!r}); "
                      f"{role!r} falls back to {self.font_name!r}")
        return resolved

    def _role_font(self, role: str) -> str:
        return self.brand_fonts.get(role, self.font_name)

    def render(self, state: AppState, profile: RenderProfile | None = None) -> bytes:
        profile = profile or BOARDS.default
        q = self.Quartz
        self._h = profile.height
        ctx = q.CGBitmapContextCreate(
            None, profile.width, profile.height, 8, 0, self.color_space, q.kCGImageAlphaNone
        )
        q.CGContextSetGrayFillColor(ctx, 0.0, 1.0)
        q.CGContextFillRect(ctx, q.CGRectMake(0, 0, profile.width, profile.height))
        q.CGContextSetGrayFillColor(ctx, 1.0, 1.0)
        q.CGContextSetShouldAntialias(ctx, True)
        q.CGContextSetTextMatrix(ctx, q.CGAffineTransformIdentity)

        profile.paint(self, ctx, state, profile)

        image = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(image)))
        stride = q.CGImageGetBytesPerRow(image)
        return pack_1bpp(pixels, profile.width, profile.height, stride)

    def decode_cover(self, data: bytes, size: int) -> CoverArt | None:
        """Decode, center-crop and downscale cover art for mono and RGB565.

        Runs off the event loop (called via asyncio.to_thread). It only touches
        the immutable color space and local Core Graphics contexts, so it is safe
        alongside the main render.
        """
        q = self.Quartz
        from Foundation import NSData  # type: ignore

        ns_data = NSData.dataWithBytes_length_(data, len(data))
        src = q.CGImageSourceCreateWithData(ns_data, None)
        if src is None or q.CGImageSourceGetCount(src) < 1:
            return None
        image = q.CGImageSourceCreateImageAtIndex(src, 0, None)
        if image is None:
            return None
        iw = int(q.CGImageGetWidth(image))
        ih = int(q.CGImageGetHeight(image))
        if iw <= 0 or ih <= 0:
            return None
        side = min(iw, ih)
        crop = q.CGImageCreateWithImageInRect(
            image, q.CGRectMake((iw - side) // 2, (ih - side) // 2, side, side)
        )
        # A fixed RGBA byte order gives us stable channel positions across
        # architectures. The same decoded pixels feed the legacy 1-bit dither
        # and the ESP8266's true-color overlay.
        bitmap_info = q.kCGImageAlphaPremultipliedLast | q.kCGBitmapByteOrder32Big
        ctx = q.CGBitmapContextCreate(
            None, size, size, 8, size * 4, self.rgb_color_space, bitmap_info
        )
        if ctx is None:
            return None
        q.CGContextSetInterpolationQuality(ctx, q.kCGInterpolationHigh)
        q.CGContextDrawImage(ctx, q.CGRectMake(0, 0, size, size), crop or image)
        out = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(out)))
        stride = int(q.CGImageGetBytesPerRow(out))
        # Core Graphics bitmap memory is top-origin (row 0 = top), matching the
        # frame buffer, so no vertical flip is needed here.
        gray: list[int] = []
        rgb565 = bytearray(size * size * 2)
        dst = 0
        for y in range(size):
            row = y * stride
            for x in range(size):
                src_px = row + x * 4
                r, g, b = pixels[src_px], pixels[src_px + 1], pixels[src_px + 2]
                gray.append((77 * r + 150 * g + 29 * b) >> 8)
                color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                rgb565[dst] = color >> 8
                rgb565[dst + 1] = color & 0xFF
                dst += 2
        return CoverArt(
            size=size,
            bits=floyd_steinberg_1bit(gray, size, size),
            rgb565=bytes(rgb565),
        )

    def render_pause_chip(self, profile: RenderProfile) -> tuple[bytes, bytes, int, int]:
        """Render the small PAUSED chip stamped over paused full-screen art.

        Returns (value, mask, chip_w, chip_h): two 1-bpp row-packed bitmaps of
        the same size. ``value`` is the white pill with black label; ``mask`` is
        the pill's coverage (lit = inside pill). The daemon composites them over
        the album art so the pill's rounded corners keep the photo behind them.
        """
        ct = self.CoreText
        font_name = self._role_font("label")
        chip = profile.pause_chip
        size, pad_x, chip_h, rise = chip.size, chip.pad_x, chip.chip_h, chip.rise
        text = "PAUSED"
        font = ct.CTFontCreateWithName(font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
            ct.kCTKernAttributeName: TRACKING_MEGA * size,
        }
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        _left, text_w, _advance = self._line_visual_metrics(line)
        chip_w = int(text_w) + pad_x * 2
        value = self._render_chip_layer(text, size, pad_x, rise, chip_w, chip_h, font_name, True)
        mask = self._render_chip_layer(text, size, pad_x, rise, chip_w, chip_h, font_name, False)
        return value, mask, chip_w, chip_h

    def _render_chip_layer(
        self,
        text: str,
        size: int,
        pad_x: int,
        rise: int,
        chip_w: int,
        chip_h: int,
        font_name: str,
        with_text: bool,
    ) -> bytes:
        q = self.Quartz
        ctx = q.CGBitmapContextCreate(
            None, chip_w, chip_h, 8, 0, self.color_space, q.kCGImageAlphaNone
        )
        q.CGContextSetGrayFillColor(ctx, 0.0, 1.0)
        q.CGContextFillRect(ctx, q.CGRectMake(0, 0, chip_w, chip_h))
        q.CGContextSetShouldAntialias(ctx, True)
        q.CGContextSetTextMatrix(ctx, q.CGAffineTransformIdentity)
        prev_h = self._h
        self._h = chip_h
        try:
            q.CGContextSetGrayFillColor(ctx, 1.0, 1.0)
            q.CGContextAddPath(ctx, self._pill_path(0, 0, chip_w, chip_h))
            q.CGContextFillPath(ctx)
            if with_text:
                self._draw_text(
                    ctx, text, size, pad_x, rise, chip_w, "left",
                    font_name=font_name, tracking=TRACKING_MEGA, gray=0.0,
                )
        finally:
            self._h = prev_h
        image = q.CGBitmapContextCreateImage(ctx)
        pixels = bytes(q.CGDataProviderCopyData(q.CGImageGetDataProvider(image)))
        stride = q.CGImageGetBytesPerRow(image)
        return pack_1bpp(pixels, chip_w, chip_h, stride)

    def _draw_lyric_band(self, ctx: Any, state: AppState, profile: RenderProfile) -> LyricLayout:
        lyric_font = self._role_font("lyric")
        current, _next_line, _third = state.current_lines()
        current_layout = fit_lyric_layout(
            current,
            profile.lyric_box_width,
            profile.lyric_band_bottom_y - profile.lyric_sizes.band_top_y,
            profile.lyric_sizes,
            self._break_text,
        )
        # Karaoke fill only makes sense for timed lines; loading/error/plain and
        # "Instrumental" copy would otherwise render fully hollow at fraction 0.
        karaoke = bool(state.lyrics.synced) and not state.lyrics.instrumental
        fraction = state.current_line_highlight_fraction() if karaoke else 1.0
        total_chars = sum(len(row) for row in current_layout.rows) or 1
        consumed_chars = 0
        for row, baseline_y in zip(current_layout.rows, current_layout.baselines):
            row_fraction = clamp(
                (fraction * total_chars - consumed_chars) / max(1, len(row)), 0.0, 1.0
            )
            self._draw_karaoke_text(
                ctx,
                row,
                current_layout.font_size,
                profile.lyric_box_x,
                baseline_y,
                profile.lyric_box_width,
                row_fraction if karaoke else 1.0,
                lyric_font,
            )
            consumed_chars += len(row)
        return current_layout

    def _break_text(self, text: str, size: float, width: int) -> list[str]:
        if not text:
            return []
        ct = self.CoreText
        font = ct.CTFontCreateWithName(self._role_font("display"), size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        ns_text = self.NSString.stringWithString_(text)
        attributed = self.NSAttributedString.alloc().initWithString_attributes_(ns_text, attrs)
        typesetter = ct.CTTypesetterCreateWithAttributedString(attributed)
        rows: list[str] = []
        offset = 0
        text_len = int(ns_text.length())
        while offset < text_len:
            count = int(ct.CTTypesetterSuggestLineBreak(typesetter, offset, float(width)))
            if count <= 0:
                count = 1
            end = min(text_len, offset + count)
            rows.append(str(ns_text.substringWithRange_((offset, end - offset))))
            offset = end
            while offset < text_len:
                ch = ns_text.characterAtIndex_(offset)
                if not (ch if isinstance(ch, str) else chr(ch)).isspace():
                    break
                offset += 1
        return rows

    def _draw_text(
        self,
        ctx: Any,
        text: str,
        size: float,
        x: int,
        baseline_y_top_origin: int,
        width: int,
        align: str,
        font_name: str | None = None,
        tracking: float = 0.0,
        gray: float = 1.0,
    ) -> None:
        if not text:
            return
        ct = self.CoreText
        q = self.Quartz
        font = ct.CTFontCreateWithName(font_name or self.font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        if tracking:
            attrs[ct.kCTKernAttributeName] = tracking * size
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        measured = ct.CTLineGetTypographicBounds(line, None, None, None)
        text_width = measured[0] if isinstance(measured, tuple) else measured
        probe = q.CGBitmapContextCreate(None, 1, 1, 8, 1, self.color_space, q.kCGImageAlphaNone)
        q.CGContextSetTextMatrix(probe, q.CGAffineTransformIdentity)
        ink = ct.CTLineGetImageBounds(line, probe)
        ink_x = ink.origin.x
        ink_y = ink.origin.y
        ink_w = ink.size.width
        ink_h = ink.size.height
        if not (ink_w > 0 and ink_h > 0):
            ascent = ct.CTFontGetAscent(font)
            descent = ct.CTFontGetDescent(font)
            ink_x = 0.0
            ink_y = -descent
            ink_w = text_width
            ink_h = ascent + descent

        visual_left = min(0.0, ink_x)
        visual_right = max(float(text_width), ink_x + ink_w)
        visual_width = max(0.0, visual_right - visual_left)
        if align == "center":
            visual_x = x + max(0, int((width - visual_width) / 2))
        elif align == "right":
            visual_x = x + max(0, int(width - visual_width))
        else:
            visual_x = x
        draw_x = visual_x - visual_left

        baseline_y = self._h - baseline_y_top_origin
        pad = 3
        clip_y = max(0, math.floor(baseline_y + ink_y - pad))
        clip_top = min(self._h, math.ceil(baseline_y + ink_y + ink_h + pad))
        if clip_top <= clip_y:
            return

        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayFillColor(ctx, gray, 1.0)
        q.CGContextClipToRect(ctx, q.CGRectMake(x, clip_y, width, clip_top - clip_y))
        q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
        ct.CTLineDraw(line, ctx)
        q.CGContextRestoreGState(ctx)

    def _draw_rule(self, ctx: Any, x: int, y_top_origin: int, width: int) -> None:
        q = self.Quartz
        y = self._h - y_top_origin
        q.CGContextFillRect(ctx, q.CGRectMake(x, y, width, 1))

    def _pill_path(self, x: float, y: float, w: float, h: float) -> Any:
        q = self.Quartz
        radius = min(h, w) / 2.0
        return q.CGPathCreateWithRoundedRect(q.CGRectMake(x, y, w, h), radius, radius, None)

    def _fill_pill(self, ctx: Any, x: float, y: float, w: float, h: float) -> None:
        if w <= 0 or h <= 0:
            return
        q = self.Quartz
        q.CGContextAddPath(ctx, self._pill_path(x, y, w, h))
        q.CGContextFillPath(ctx)

    def _stroke_pill(self, ctx: Any, x: float, y: float, w: float, h: float) -> None:
        if w <= 0 or h <= 0:
            return
        q = self.Quartz
        q.CGContextAddPath(ctx, self._pill_path(x, y, w, h))
        q.CGContextStrokePath(ctx)

    def _draw_cover_card(self, ctx: Any, place: "CoverPlacement") -> None:
        """Tend hairline card frame around the album art (radius-8 rounded rect).

        Drawn in the pre-invert pass as set bits, so after the light-theme flip
        it reads as a 1px ink hairline on paper -- the same card chrome the other
        Tend screens use (u8g2 DrawRFrame, radius 8). The art is blitted inside
        this frame later, in _render, so it keeps its natural tonality.
        """
        q = self.Quartz
        pad = place.frame_pad
        fx = place.x - pad
        fw = place.size + 2 * pad
        fy_top = place.y - pad
        rect = q.CGRectMake(fx, self._h - fy_top - fw, fw, fw)
        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        q.CGContextAddPath(ctx, q.CGPathCreateWithRoundedRect(rect, 8.0, 8.0, None))
        q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)

    def _line_visual_metrics(self, line: Any) -> tuple[float, float, float]:
        """Return (visual_left, visual_width, advance_width) for a CTLine.

        visual_left/width mirror the union of advance width and image bounds
        that _draw_text uses for alignment.
        """
        ct = self.CoreText
        q = self.Quartz
        measured = ct.CTLineGetTypographicBounds(line, None, None, None)
        text_width = measured[0] if isinstance(measured, tuple) else measured
        probe = q.CGBitmapContextCreate(None, 1, 1, 8, 1, self.color_space, q.kCGImageAlphaNone)
        q.CGContextSetTextMatrix(probe, q.CGAffineTransformIdentity)
        ink = ct.CTLineGetImageBounds(line, probe)
        ink_x, ink_w = ink.origin.x, ink.size.width
        if not (ink_w > 0):
            ink_x, ink_w = 0.0, float(text_width)
        visual_left = min(0.0, ink_x)
        visual_right = max(float(text_width), ink_x + ink_w)
        return visual_left, max(0.0, visual_right - visual_left), float(text_width)

    def _draw_karaoke_text(
        self,
        ctx: Any,
        text: str,
        size: float,
        x: int,
        baseline_y_top_origin: int,
        width: int,
        fraction: float,
        font_name: str,
    ) -> None:
        """Centered lyric row: sung portion solid, unsung portion hollow outline."""
        if not text:
            return
        ct = self.CoreText
        q = self.Quartz
        font = ct.CTFontCreateWithName(font_name, size, None)
        solid_attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
        }
        # Stroke-only text ignores the context fill color; without an explicit
        # CGColor stroke attribute Core Text draws nothing here.
        hollow_attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTStrokeWidthAttributeName: max(KARAOKE_STROKE_PCT, 100.0 / size),
            ct.kCTStrokeColorAttributeName: q.CGColorCreateGenericGray(1.0, 1.0),
        }
        solid_line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, solid_attrs)
        )
        hollow_line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, hollow_attrs)
        )
        visual_left, visual_width, _advance = self._line_visual_metrics(solid_line)
        visual_x = x + max(0, int((width - visual_width) / 2))
        draw_x = visual_x - visual_left
        baseline_y = self._h - baseline_y_top_origin
        # The clip only bounds the horizontal wipe; keep it tall so Thai mark
        # stacks and deep descenders never get shaved (only this row's glyphs
        # are drawn in this pass, so a generous band cannot bleed).
        band_h = size * 2.4
        band_y = baseline_y - size * 0.8

        fraction = clamp(fraction, 0.0, 1.0)
        split_x = visual_x + visual_width * fraction

        q.CGContextSaveGState(ctx)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        if fraction < 1.0:
            q.CGContextSaveGState(ctx)
            q.CGContextClipToRect(ctx, q.CGRectMake(split_x, band_y, x + width - split_x, band_h))
            q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
            ct.CTLineDraw(hollow_line, ctx)
            q.CGContextRestoreGState(ctx)
        if fraction > 0.0:
            q.CGContextSaveGState(ctx)
            q.CGContextClipToRect(ctx, q.CGRectMake(x, band_y, split_x - x, band_h))
            q.CGContextSetTextPosition(ctx, draw_x, baseline_y)
            ct.CTLineDraw(solid_line, ctx)
            q.CGContextRestoreGState(ctx)
        q.CGContextRestoreGState(ctx)

    def _screen_rect(self, ctx: Any, x: int, y_top_origin: int, w: int, h: int) -> None:
        """Punch a 50% checkerboard of background over a band.

        On the 1-bit panel this halftone-screens whatever was drawn there, so
        the text underneath reads as gray -- the design system's ink-muted.
        """
        q = self.Quartz
        y0 = self._h - y_top_origin - h
        q.CGContextSaveGState(ctx)
        q.CGContextSetShouldAntialias(ctx, False)
        q.CGContextSetGrayStrokeColor(ctx, 0.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        for row in range(h):
            q.CGContextSetLineDash(ctx, float((row + x) % 2), [1.0, 1.0], 2)
            q.CGContextBeginPath(ctx)
            q.CGContextMoveToPoint(ctx, float(x), y0 + row + 0.5)
            q.CGContextAddLineToPoint(ctx, float(x + w), y0 + row + 0.5)
            q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)

    def _draw_state_chip(
        self,
        ctx: Any,
        text: str,
        x: int,
        baseline_y_top_origin: int,
        font_name: str,
        size: int = 13,
        pad_x: int = 9,
        chip_h: int = 20,
        rise: int = 14,
    ) -> None:
        """Inverted pill chip -- the 1-bit stand-in for the ember accent."""
        ct = self.CoreText
        font = ct.CTFontCreateWithName(font_name, size, None)
        attrs = {
            ct.kCTFontAttributeName: font,
            ct.kCTForegroundColorFromContextAttributeName: True,
            ct.kCTKernAttributeName: TRACKING_MEGA * size,
        }
        line = ct.CTLineCreateWithAttributedString(
            self.NSAttributedString.alloc().initWithString_attributes_(text, attrs)
        )
        _left, text_w, _advance = self._line_visual_metrics(line)
        chip_w = int(text_w) + pad_x * 2
        chip_y_top = baseline_y_top_origin - rise
        self._fill_pill(ctx, x, self._h - chip_y_top - chip_h, chip_w, chip_h)
        self._draw_text(
            ctx, text, size, x + pad_x, baseline_y_top_origin, chip_w, "left",
            font_name=font_name, tracking=TRACKING_MEGA, gray=0.0,
        )

    def _draw_progress(self, ctx: Any, state: AppState, font_name: str, geom: ProgressGeom) -> None:
        q = self.Quartz
        elapsed = state.clock.interpolated_position()
        duration = max(0.0, state.track.duration_sec)
        self._draw_text(
            ctx, format_time(elapsed), geom.time_size, geom.left_x, geom.baseline_y, geom.left_w,
            "left", font_name=font_name,
        )
        remaining = max(0.0, duration - elapsed) if duration else 0.0
        self._draw_text(
            ctx, "-" + format_time(remaining), geom.time_size, geom.right_x, geom.baseline_y, geom.right_w,
            "right", font_name=font_name,
        )
        bar_x, bar_w = geom.bar_x, geom.bar_w
        center_y = self._h - geom.center_y  # track centerline, bottom-up coords
        fraction = clamp(elapsed / duration, 0.0, 1.0) if duration > 0 else 0.0
        played_w = bar_w * fraction
        # remaining track: dotted hairline reads as ink-muted on the 1-bit panel
        q.CGContextSaveGState(ctx)
        q.CGContextSetShouldAntialias(ctx, False)
        q.CGContextSetGrayStrokeColor(ctx, 1.0, 1.0)
        q.CGContextSetLineWidth(ctx, 1.0)
        q.CGContextSetLineDash(ctx, 0.0, [1.0, 3.0], 2)
        q.CGContextBeginPath(ctx)
        q.CGContextMoveToPoint(ctx, bar_x + played_w, center_y - 0.5)
        q.CGContextAddLineToPoint(ctx, bar_x + bar_w, center_y - 0.5)
        q.CGContextStrokePath(ctx)
        q.CGContextRestoreGState(ctx)
        # played track: solid bar, then the playhead dot on top
        if played_w > 0:
            self._fill_pill(ctx, bar_x, center_y - 1.5, played_w, 3)
        dot_r = geom.dot_r
        q.CGContextFillEllipseInRect(
            ctx, q.CGRectMake(bar_x + played_w - dot_r, center_y - dot_r, dot_r * 2, dot_r * 2)
        )


class FallbackFrameRenderer(FrameRenderer):
    def render(self, state: AppState, profile: RenderProfile | None = None) -> bytes:
        width, height = profile.width, profile.height
        frame = bytearray(profile.frame_bytes)
        elapsed = state.clock.interpolated_position()
        duration = max(1.0, state.track.duration_sec or 1.0)
        geom = profile.progress_geom
        fill = int(clamp(elapsed / duration, 0.0, 1.0) * geom.bar_w)
        draw_rect(frame, geom.bar_x, geom.center_y - 5, geom.bar_w, 10, width, height)
        if fill:
            fill_rect(frame, geom.bar_x + 1, geom.center_y - 4, fill, 8, width, height)
        fill_rect(frame, 12, 27 * height // DISPLAY_HEIGHT, width - 24, 1, width, height)
        fill_rect(frame, 12, 130 * height // DISPLAY_HEIGHT, width - 24, 1, width, height)
        return bytes(frame)

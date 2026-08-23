"""Render what radar_screen.cpp will draw, from live data, on a Mac.

This mirrors the shipped C: the same projection and crop, the same tier
classifier and Bayer densities, the same ray-based beam, the same furniture and
panel geometry. It exists to catch layout mistakes -- labels running off the
scope, panel text colliding, a beam sweeping the wrong way -- without a board.

Not pixel-exact for TEXT: u8g2's bitmap fonts are approximated with whatever
TrueType face is to hand. Glyph ADVANCES are taken from the real u8g2 font
headers, so fit and overflow are checked honestly even though the letterforms
differ. Everything that is not text is exact.

Run:  python3 tools/radar_preview.py [--out DIR] [--seconds N]
"""

import argparse
import io
import json
import math
import os
import time
import urllib.request

from PIL import Image, ImageDraw, ImageFont

# ---- constants, kept in step with firmware/main/radar_screen.cpp ----------
HOME_LAT, HOME_LON = 15.3919001, 99.8456348
ZOOM, TILE_PX = 7, 512
SCOPE, CENTER, RADIUS = 300, 150, 148
ROW_BYTES = (SCOPE + 7) // 8
PANEL_X, PANEL_W = 300, 100
# Derived from ZOOM, mirroring radar_screen_init(). A 512 px tile covers the
# same ground as a 256 px one at the same zoom, so its pixels are half the size.
KM_PER_PX = 156543.03392 * math.cos(math.radians(HOME_LAT)) / (1 << ZOOM) / (TILE_PX / 256) / 1000.0
RANGE_KM = round(RADIUS * KM_PER_PX)

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
TIER_DENSITY = [2, 4, 8, 16]
TIER_FLOOR = 0  # light and above; tier 0 is stippled at 2/16

BEAM_RAYS, BEAM_STEP_DEG, BEAM_MAX_DENSITY = 100, 0.4, 7
STALE_AGE_SEC = 25 * 60

# Coordinates, not pixels -- mirrors RADAR_LANDMARKS. Projected at render time
# against the same crop origin as the firmware, so they follow ZOOM.
LANDMARKS = [
    (15.7047, 100.1372, "NAKHON SAWAN"),
    (15.3794, 100.0245, "UTHAI THANI"),
    (14.8879, 100.4046, "SING BURI"),
    (14.8418, 99.6976, "DAN CHANG"),
    (15.4529, 99.5761, "LAN SAK"),
]

# Real u8g2 advances, read out of the font headers (max_char_width).
U8G2_ADVANCE = {"5x7": 5, "6x12": 6, "logisoso28": 16, "helvB10": 8, "helvB14": 10}


def project(lat, lon):
    span = float(1 << ZOOM) * TILE_PX
    lat_rad = math.radians(lat)
    return (
        (lon + 180.0) / 360.0 * span,
        (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * span,
    )


def tier(r, g, b):
    if b > g and b >= r:
        if g >= 200:
            return 0
        return 1 if g >= 160 else 2
    return 3  # not the blue ramp means the warm one, and all of it is heavy


def font(px):
    for path in (
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ):
        try:
            return ImageFont.truetype(path, px)
        except OSError:
            continue
    return ImageFont.load_default()


def u8g2_width(text, face):
    return len(text) * U8G2_ADVANCE[face]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=".")
    parser.add_argument("--seconds", type=float, default=None,
                        help="second-into-minute for the beam (default: now)")
    parser.add_argument("--stale", type=int, default=None, metavar="MIN",
                        help="pretend the newest frame is MIN minutes old, to "
                             "exercise the stopped-beam and frame-age path")
    args = parser.parse_args()

    home_x, home_y = project(HOME_LAT, HOME_LON)
    tile_x, tile_y = int(round(home_x)) // TILE_PX, int(round(home_y)) // TILE_PX
    crop_x = int(round(home_x)) - tile_x * TILE_PX - CENTER
    crop_y = int(round(home_y)) - tile_y * TILE_PX - CENTER
    print(f"tile /{TILE_PX}/{ZOOM}/{tile_x}/{tile_y}/  crop x {crop_x}..{crop_x+SCOPE} y {crop_y}..{crop_y+SCOPE}")

    index = json.load(urllib.request.urlopen("https://api.rainviewer.com/public/weather-maps.json", timeout=20))
    newest = index["radar"]["past"][-1]
    url = f'{index["host"]}{newest["path"]}/{TILE_PX}/{ZOOM}/{tile_x}/{tile_y}/0/0_0.png'
    print(f"frame {newest['time']} ({int(time.time()) - newest['time']}s old)  {url}")
    tile = Image.open(io.BytesIO(urllib.request.urlopen(url, timeout=30).read())).convert("RGBA")

    forecast = json.load(urllib.request.urlopen(
        "https://api.open-meteo.com/v1/forecast?latitude=15.3919&longitude=99.8456"
        "&hourly=precipitation_probability&current=precipitation&forecast_hours=8"
        "&timezone=Asia%2FBangkok", timeout=20))
    probability = forecast["hourly"]["precipitation_probability"]
    p3, p7 = probability[3], probability[7]
    raining_home = forecast["current"]["precipitation"] > 0
    print(f"+3H {p3}%  +7H {p7}%  current {forecast['current']['precipitation']} mm")

    img = Image.new("1", (400, 300), 0)
    px = img.load()
    draw = ImageDraw.Draw(img)

    # --- rain, exactly as radar_png_draw does it ---------------------------
    src = tile.load()
    ink = 0
    inside_total = 0
    for sy in range(SCOPE):
        dy = sy - CENTER
        for sx in range(SCOPE):
            dx = sx - CENTER
            if dx * dx + dy * dy > RADIUS * RADIUS:
                continue
            inside_total += 1
            r, g, b, a = src[crop_x + sx, crop_y + sy]
            if a != 255:
                continue
            t = tier(r, g, b)
            if t < TIER_FLOOR:
                continue
            if BAYER[sy & 3][sx & 3] < TIER_DENSITY[t]:
                px[sx, sy] = 1
                ink += 1

    # --- beam --------------------------------------------------------------
    age_sec = args.stale * 60 if args.stale is not None else int(time.time()) - newest["time"]
    live = age_sec < STALE_AGE_SEC
    # Stale imagery parks the beam at 12 o'clock, which is what stops it; it is
    # never simply absent.
    seconds = (args.seconds if args.seconds is not None else time.time() % 60) if live else 0.0
    if True:
        lead_deg = -90.0 + seconds * 6.0
        for i in range(BEAM_RAYS):
            density = 16 if i == 0 else -(-BEAM_MAX_DENSITY * (BEAM_RAYS - i) // BEAM_RAYS)
            if density <= 0:
                continue
            angle = math.radians(lead_deg - i * BEAM_STEP_DEG)
            ca, sa = math.cos(angle), math.sin(angle)
            for r in range(1, RADIUS + 1):
                x = CENTER + int(ca * r)
                y = CENTER + int(sa * r)
                if BAYER[y & 3][x & 3] < density:
                    px[x, y] = 1

    # --- furniture ---------------------------------------------------------
    for radius in range(37, RADIUS + 1, 37):
        draw.ellipse([CENTER - radius, CENTER - radius, CENTER + radius, CENTER + radius], outline=1)
    draw.line([CENTER - RADIUS, CENTER, CENTER + RADIUS - 1, CENTER], fill=1)
    draw.line([CENTER, CENTER - RADIUS, CENTER, CENTER + RADIUS - 1], fill=1)
    for bearing in range(0, 360, 15):
        angle = math.radians(bearing)
        length = 8 if bearing % 45 == 0 else 4
        ca, sa = math.cos(angle), math.sin(angle)
        draw.line([CENTER + int((RADIUS - length) * ca), CENTER + int((RADIUS - length) * sa),
                   CENTER + int(RADIUS * ca), CENTER + int(RADIUS * sa)], fill=1)

    # --- landmarks ---------------------------------------------------------
    small = font(7)
    problems = []
    for mark_lat, mark_lon, label in LANDMARKS:
        # Same origin as the firmware: tile_x * TILE_PX + crop_x reduces to
        # round(home_x) - CENTER, so a landmark lands relative to home.
        mark_x, mark_y = project(mark_lat, mark_lon)
        lx = round(mark_x - (tile_x * TILE_PX + crop_x))
        ly = round(mark_y - (tile_y * TILE_PX + crop_y))
        if (lx - CENTER) ** 2 + (ly - CENTER) ** 2 > RADIUS * RADIUS:
            problems.append(f"{label}: off the scope at zoom {ZOOM}")
            continue
        width = u8g2_width(label, "5x7")
        # Mirrors draw_landmarks(): the label goes on the side AWAY from home,
        # so its knockout never blanks the corridor rain crosses on approach.
        outward_left = lx < CENTER
        label_x = lx - 5 - width if outward_left else lx + 5
        flipped = False
        if label_x < 2 or label_x + width > SCOPE - 2:
            label_x = lx + 5 if outward_left else lx - 5 - width
            flipped = True
        if label_x < 2 or label_x + width > SCOPE - 2:
            problems.append(f"{label}: does not fit on either side at x={label_x}")
        draw.rectangle([label_x - 1, ly - 4, label_x + width, ly + 4], fill=0)
        draw.rectangle([lx - 2, ly - 2, lx + 2, ly + 2], fill=0)
        draw.rectangle([lx - 1, ly - 1, lx + 1, ly + 1], outline=1)
        draw.text((label_x, ly - 4), label, font=small, fill=1)
        print(f"  {label:<14} dot ({lx:3d},{ly:3d})  label x {label_x:3d}..{label_x+width:3d}"
              f"{'  [flipped]' if flipped else ''}")

    # --- home marker -------------------------------------------------------
    if raining_home:
        draw.ellipse([CENTER - 3, CENTER - 3, CENTER + 3, CENTER + 3], fill=1)
    else:
        draw.ellipse([CENTER - 3, CENTER - 3, CENTER + 3, CENTER + 3], outline=1)
    draw.line([CENTER - 7, CENTER, CENTER + 7, CENTER], fill=1)
    draw.line([CENTER, CENTER - 7, CENTER, CENTER + 7], fill=1)

    # --- panel -------------------------------------------------------------
    draw.line([PANEL_X, 0, PANEL_X, SCOPE - 1], fill=1)
    eyebrow, big, pct = font(11), font(34), font(11)

    def centered(text, face, baseline, pil_font):
        width = u8g2_width(text, face)
        draw.text((PANEL_X + (PANEL_W - width) // 2, baseline - pil_font.size), text, font=pil_font, fill=1)
        return width

    def probability(baseline, value):
        digits = str(value)
        digits_w = u8g2_width(digits, "logisoso28")
        sign_w = u8g2_width("%", "helvB10")
        total = digits_w + 2 + sign_w
        x = PANEL_X + (PANEL_W - total) // 2
        if x < PANEL_X + 2:
            problems.append(f"panel number {value}% is {total}px wide, overflows the {PANEL_W}px panel")
        draw.text((x, baseline - 28), digits, font=big, fill=1)
        draw.text((x + digits_w + 2, baseline - 11), "%", font=pct, fill=1)
        return total

    centered("+3H", "6x12", 60, eyebrow)
    w3 = probability(102, p3)
    draw.line([PANEL_X + 12, 140, PANEL_X + PANEL_W - 12, 140], fill=1)
    centered("+7H", "6x12", 180, eyebrow)
    w7 = probability(222, p7)
    # Capture time of the frame on screen, straight from the RainViewer stamp.
    stamp = time.strftime("%H:%M", time.localtime(newest["time"]))
    centered("DATA", "5x7", 258, font(7))
    centered(stamp, "6x12", 272, font(11))
    footer = f"{RANGE_KM} KM" if live else f"-{min(age_sec // 60, 999)}M"
    centered(footer, "5x7", 290, font(7))

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "radar_preview.png")
    img.save(path)
    img.resize((1200, 900), Image.NEAREST).save(os.path.join(args.out, "radar_preview_big.png"))

    print(f"\nscope ink {100*ink/inside_total:.0f}% of the scope area")
    print(f"panel numbers: +3H {w3}px, +7H {w7}px (panel is {PANEL_W}px)")
    print(f"beam at {seconds:.1f}s -> lead {(-90.0 + seconds*6.0) % 360:.0f} deg "
          f"({'live' if live else 'FROZEN, frame is stale'})")
    print(f"frame age {age_sec//60}m -> footer reads {footer!r}, DATA stamp {stamp}")
    if problems:
        print("\nPROBLEMS:")
        for problem in problems:
            print("  -", problem)
    else:
        print("\nno layout problems detected")
    print(f"wrote {path}")


if __name__ == "__main__":
    main()

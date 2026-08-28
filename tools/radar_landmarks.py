"""Pick and project the radar-scope landmarks.

The scope (radar_screen.cpp) draws a handful of labelled dots so the rain has
something to sit against instead of an empty field. This script picks them:
it resolves candidate town names to coordinates through Open-Meteo's keyless
geocoding API, projects them with the same Web Mercator math the firmware
uses, and keeps only the ones that land inside the scope.

Coordinates are never typed in by hand -- only names are, and a name that
resolves to the wrong place shows up immediately as a bad bearing/range.

Run:  python3 tools/radar_landmarks.py
Emits the C table for RADAR_LANDMARKS.
"""

import json
import math
import urllib.parse
import urllib.request

HOME_LAT = 15.3919001
HOME_LON = 99.8456348
# Only affects the km/bearing figures printed for reading. The emitted table is
# coordinates, and the FIRMWARE now does the picking at init, so this no longer
# has to agree with RADAR_ZOOM for the output to be correct.
ZOOM = 7
TILE_PX = 512
SCOPE = 300
SCOPE_R = 148  # px; the drawn scope radius
MIN_KM = 10.0  # anything nearer collides with the centre marker
MIN_SEPARATION_PX = 60  # keep the fifth label clear of the quadrant four
MAX_LABEL_CHARS = 13  # 5x7 font: 13 chars is ~65 px, the widest that fits

# Names only. Every coordinate below comes back from the API.
CANDIDATES = [
    "Nakhon Sawan",
    "Uthai Thani",
    "Chai Nat",
    "Kamphaeng Phet",
    "Suphan Buri",
    "Lop Buri",
    "Phichit",
    "Sing Buri",
    "Ang Thong",
    "Tak",
    "Takhli",
    "Ban Rai",
    "Lan Sak",
    "Phayuha Khiri",
    "Khanu Woralaksaburi",
    "Dan Chang",
    "Nong Chang",
    "Sawankhalok",
    "Phitsanulok",
    "Saraburi",
]


def project(lat, lon):
    """Web Mercator -> global pixel coordinates at ZOOM, in TILE_PX tiles.

    Mirrors radar_project() in firmware/main/radar_screen.cpp. Keep the two in
    step: this is the only place the projection is checked against real data.
    """
    n = float(1 << ZOOM)
    x = (lon + 180.0) / 360.0 * n
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n
    return x * TILE_PX, y * TILE_PX


def geocode(name):
    url = "https://geocoding-api.open-meteo.com/v1/search?" + urllib.parse.urlencode(
        {"name": name, "count": 5, "language": "en", "format": "json"}
    )
    with urllib.request.urlopen(url, timeout=20) as response:
        payload = json.load(response)
    for hit in payload.get("results", []):
        # Towns only, and only in Thailand: "Tak" and "Chai Nat" both match
        # places elsewhere, and the airport entries sit next to the town ones.
        if hit.get("country_code") != "TH":
            continue
        if not str(hit.get("feature_code", "")).startswith("PPL"):
            continue
        return hit
    return None


def main():
    home_x, home_y = project(HOME_LAT, HOME_LON)
    # The firmware crops so that home lands exactly at the scope centre.
    crop_x = round(home_x) - SCOPE // 2
    crop_y = round(home_y) - SCOPE // 2
    print(f"home global px : {home_x:.2f}, {home_y:.2f}")
    print(f"tile           : /{TILE_PX}/{ZOOM}/{int(home_x // TILE_PX)}/{int(home_y // TILE_PX)}/")
    print(f"home in tile   : {home_x % TILE_PX:.2f}, {home_y % TILE_PX:.2f}")
    print(f"crop in tile   : x {crop_x % TILE_PX}..{crop_x % TILE_PX + SCOPE}, "
          f"y {crop_y % TILE_PX}..{crop_y % TILE_PX + SCOPE}")

    metres_per_px = 156543.03392 * math.cos(math.radians(HOME_LAT)) / (1 << ZOOM) / (TILE_PX / 256)
    print(f"!! firmware must read: static const int RADAR_ZOOM = {ZOOM};")
    print(f"scale          : {metres_per_px / 1000:.3f} km/px, "
          f"scope radius {SCOPE_R * metres_per_px / 1000:.1f} km\n")

    # Everything that geocodes goes in the pool, whether or not it lands inside
    # the scope AT THIS ZOOM. Filtering here would starve a wider zoom of the
    # towns it needs, and the firmware discards out-of-range ones at init
    # anyway. `kept` is only for the readout below.
    resolved = []
    kept = []
    for name in CANDIDATES:
        hit = geocode(name)
        if hit is None:
            print(f"  {name:<22} no PPL hit in TH")
            continue
        gx, gy = project(hit["latitude"], hit["longitude"])
        px = gx - crop_x
        py = gy - crop_y
        dx = px - SCOPE / 2
        dy = py - SCOPE / 2
        dist = math.hypot(dx, dy)
        km = dist * metres_per_px / 1000
        bearing = (math.degrees(math.atan2(dx, -dy))) % 360
        inside = dist <= SCOPE_R - 8  # keep the label off the rim
        flag = "keep" if inside else "  --"
        print(f"  {flag} {name:<22} {hit['latitude']:8.4f},{hit['longitude']:9.4f}  "
              f"px ({px:6.1f},{py:6.1f})  {km:5.1f} km  {bearing:5.1f} deg  "
              f"pop {hit.get('population', 0)}")
        resolved.append((name, hit))
        if inside:
            kept.append((name, hit, round(px), round(py), km, bearing))

    print(f"\n{len(resolved)} resolved, {len(kept)} inside the scope at zoom {ZOOM}\n")

    # Emits the whole POOL. Choosing which five to draw moved onto the board
    # -- it depends only on RADAR_ZOOM, which the firmware knows and this
    # script does not have to be told. Re-run this only to add or drop a name.
    print("static const RadarLandmark RADAR_LANDMARKS[] = {")
    for name, hit in resolved:
        label = name.upper()
        pop = hit.get("population") or 0
        print(f'    {{{hit["latitude"]:8.4f}, {hit["longitude"]:9.4f}, {pop:7d}, "{label}"}},')
    print("};")


if __name__ == "__main__":
    main()

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
    print(f"scale          : {metres_per_px / 1000:.3f} km/px, "
          f"scope radius {SCOPE_R * metres_per_px / 1000:.1f} km\n")

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
        if inside:
            kept.append((name, hit, round(px), round(py), km, bearing))

    # Pick for spread, not for size. Sorting by population alone piles every
    # label into the east/south-east (Nakhon Sawan, Uthai Thani, Chai Nat,
    # Sing Buri) and leaves half the scope with nothing to read against, so
    # take the best town per bearing quadrant first.
    kept = [row for row in kept if row[4] >= MIN_KM and len(row[0]) <= MAX_LABEL_CHARS]

    def quadrant(bearing):
        return int(((bearing + 45.0) % 360.0) // 90.0)  # 0=N 1=E 2=S 3=W

    chosen = []
    for q in range(4):
        in_q = [row for row in kept if quadrant(row[5]) == q]
        if in_q:
            chosen.append(max(in_q, key=lambda row: row[1].get("population") or 0))

    # Top up to five with the largest remaining town that is not crowding one
    # already chosen.
    remaining = sorted(
        (row for row in kept if row not in chosen),
        key=lambda row: row[1].get("population") or 0,
        reverse=True,
    )
    for row in remaining:
        if len(chosen) >= 5:
            break
        if all(math.hypot(row[2] - c[2], row[3] - c[3]) >= MIN_SEPARATION_PX for c in chosen):
            chosen.append(row)

    chosen.sort(key=lambda row: row[5])
    names = ("N", "E", "S", "W")
    print(f"\n{len(kept)} usable, {len(chosen)} chosen\n")
    print("// Generated by tools/radar_landmarks.py -- do not hand-edit coordinates.")
    print("static const RadarLandmark RADAR_LANDMARKS[] = {")
    for name, hit, px, py, km, bearing in chosen:
        label = name.upper()
        print(f'    {{{px:3d}, {py:3d}, "{label}"}},'.ljust(38) +
              f'// {names[quadrant(bearing)]}  {km:5.1f} km  {bearing:5.1f} deg  '
              f'{hit["latitude"]:.4f},{hit["longitude"]:.4f}')
    print("};")


if __name__ == "__main__":
    main()

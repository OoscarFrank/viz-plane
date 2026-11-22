#!/usr/bin/env python3
"""Generates the base maps of the selectable places and the zone table (assets/zones.c).

Each map is 800x440 RGB565, zlib-compressed, inflated into PSRAM when the place is chosen.
Mulhouse-Basel keeps its hand-made map (tools/gen_assets.py). The other places are drawn from:
- OpenStreetMap through the Overpass API (cached): coastline, motorways, rivers, runways, towns;
- Natural Earth 1:10m: lakes, urban areas, country borders.
Map labels stay in French, like the rest of the on-screen text.
Usage: uv run --with pillow --with pyshp python tools/gen_maps.py <natural-earth dir> <osm cache dir>
The Natural Earth layers used: ne_10m_lakes, ne_10m_urban_areas, ne_10m_admin_0_boundary_lines_land (unzipped).
"""
import json, math, sys, time, unicodedata, urllib.error, urllib.parse, urllib.request, zlib
from pathlib import Path
from PIL import Image, ImageDraw
import shapefile

import gen_assets as ga
from gen_assets import text, dashed, font, hexc, F_MED, F_SEMI, F_BOLD

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "components" / "radar_app" / "assets"
W, H, S = 800, 440, 3
SPAN_KM = 81.0            # width of every map, as Mulhouse-Basel
LAT_SPAN = 0.40
OVERPASS = ["https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter",
            "https://overpass.kumi.systems/api/interpreter"]

# id, title, reference city for distances (name, lat, lon), map centre, airports (IATA, city, lat, lon),
# a point at sea for the flood fill (or None), extra labels (text, lon, lat, kind)
ZONES = [
    ("bale", "Mulhouse – Bâle", ("Mulhouse", 47.7508, 7.3359), (47.67, 7.47),
     [("BSL", "Bâle-Mulhouse", 47.5896, 7.5299)], None, []),
    ("paris", "Paris", ("Paris", 48.8566, 2.3522), (48.84, 2.45),
     [("CDG", "Paris", 49.0097, 2.5479), ("ORY", "Paris", 48.7262, 2.3652), ("LBG", "Paris", 48.9694, 2.4414)], None, []),
    ("nyc", "New York", ("New York", 40.7128, -74.0060), (40.70, -73.93),
     [("JFK", "New York", 40.6413, -73.7781), ("LGA", "New York", 40.7769, -73.8740), ("EWR", "New York", 40.6895, -74.1745)],
     (40.53, -73.60), [("OCÉAN ATLANTIQUE", -73.62, 40.545, "sea")]),
    ("tlv", "Tel Aviv", ("Tel Aviv", 32.0853, 34.7818), (32.02, 34.85),
     [("TLV", "Tel Aviv", 32.0114, 34.8867)], (32.00, 34.48), [("MER MÉDITERRANÉE", 34.47, 32.12, "sea")]),
    ("dubai", "Dubaï", ("Dubaï", 25.2048, 55.2708), (25.075, 55.20),
     [("DXB", "Dubaï", 25.2528, 55.3644), ("DWC", "Dubaï", 24.8964, 55.1614)], (25.20, 55.00),
     [("GOLFE PERSIQUE", 55.02, 25.16, "sea")]),
    ("london", "Londres", ("Londres", 51.5074, -0.1278), (51.38, -0.20),
     [("LHR", "Londres", 51.4700, -0.4543), ("LGW", "Londres", 51.1537, -0.1821), ("LCY", "Londres", 51.5048, 0.0495)], None, []),
    ("geneve", "Genève", ("Genève", 46.2044, 6.1432), (46.22, 6.15),
     [("GVA", "Genève", 46.2381, 6.1090)], None,
     [("FRANCE", 5.92, 46.07, "country"), ("SUISSE", 6.40, 46.40, "country"), ("JURA", 5.95, 46.38, "relief"),
      ("SALÈVE", 6.18, 46.12, "relief"), ("LAC LÉMAN", 6.36, 46.33, "water")]),
    ("zurich", "Zurich", ("Zurich", 47.3769, 8.5417), (47.42, 8.55),
     [("ZRH", "Zurich", 47.4582, 8.5555)], None,
     [("SUISSE", 8.30, 47.30, "country"), ("ALLEMAGNE", 8.45, 47.60, "country")]),
    ("amsterdam", "Amsterdam", ("Amsterdam", 52.3676, 4.9041), (52.33, 4.85),
     [("AMS", "Amsterdam", 52.3105, 4.7683)], (52.40, 4.30), [("MER DU NORD", 4.38, 52.20, "sea")]),
    ("nice", "Nice", ("Nice", 43.7102, 7.2620), (43.70, 7.20),
     [("NCE", "Nice", 43.6584, 7.2159)], (43.55, 7.30),
     [("MER MÉDITERRANÉE", 7.25, 43.56, "sea"), ("FRANCE", 6.90, 43.85, "country"), ("ITALIE", 7.62, 43.86, "country")]),
    ("hongkong", "Hong Kong", ("Hong Kong", 22.2793, 114.1628), (22.33, 114.05),
     [("HKG", "Hong Kong", 22.3080, 113.9185)], (22.17, 113.80), [("MER DE CHINE", 114.00, 22.15, "sea")]),
]

C_LAND, C_SEA = "#152930", "#0e2029"


def extent(c):
    lat, lon = c
    lon_span = SPAN_KM / (111.32 * math.cos(math.radians(lat)))
    return lat - LAT_SPAN / 2, lat + LAT_SPAN / 2, lon - lon_span / 2, lon + lon_span / 2


def latin1(s):
    if not s: return None
    out = "".join(ch if ord(ch) <= 0xFF else "".join(c for c in unicodedata.normalize("NFKD", ch) if ord(c) < 0x80) for ch in s)
    return out.strip() if out.strip() and len(out) >= len(s) * 0.8 else None


def fold(s): return "".join(c for c in unicodedata.normalize("NFKD", s) if ord(c) < 0x80).lower()


def overpass(cache, zid, box):
    f = cache / f"{zid}.json"
    if not f.exists():
        s, n, w, e = box
        q = (f'[out:json][timeout:180][bbox:{s},{w},{n},{e}];('
             'way["natural"="coastline"];way["highway"~"^(motorway|trunk)$"];way["waterway"="river"];'
             'way["aeroway"="runway"];node["place"~"^(city|town|borough)$"];);out geom;')
        for attempt in range(6):   # public servers: retry on another one when busy
            url = OVERPASS[attempt % len(OVERPASS)]
            req = urllib.request.Request(url, data=urllib.parse.urlencode({"data": q}).encode(),
                                         headers={"User-Agent": "viz-plane map generator"})
            try:
                f.write_bytes(urllib.request.urlopen(req, timeout=240).read()); break
            except (urllib.error.URLError, TimeoutError) as e:
                print(f"{zid}: {url}: {e}, retrying"); time.sleep(10)
        else:
            raise SystemExit(f"{zid}: Overpass unavailable")
    return json.loads(f.read_text())["elements"]


def ne_shapes(ne, name, box):
    s, n, w, e = box
    r = shapefile.Reader(str(ne / name / f"{name}.shp"))
    for sh in r.iterShapes():
        x0, y0, x1, y1 = sh.bbox
        if x1 < w or x0 > e or y1 < s or y0 > n: continue
        parts = list(sh.parts) + [len(sh.points)]
        for a, b in zip(parts, parts[1:]): yield sh.points[a:b]


def draw_zone(z, ne, cache):
    zid, title, ref, centre, apts, sea, labels = z
    lat0, lat1, lon0, lon1 = extent(centre)
    X = lambda lon: (lon - lon0) / (lon1 - lon0) * W * S
    Y = lambda lat: (lat1 - lat) / (lat1 - lat0) * H * S
    P = lambda pts: [(X(p[0]), Y(p[1])) for p in pts]
    G = lambda g: [(X(p["lon"]), Y(p["lat"])) for p in g]
    km_px = W * S / SPAN_KM
    km_lon = 111.32 * math.cos(math.radians(centre[0]))
    box = (lat0 - .05, lat1 + .05, lon0 - .08, lon1 + .08)
    osm = overpass(cache, zid, box)
    ways = lambda pred: [e for e in osm if e["type"] == "way" and pred(e.get("tags", {}))]

    img = Image.new("RGBA", (W * S, H * S), hexc(C_LAND))
    d = ImageDraw.Draw(img)
    if sea:   # coastline drawn as walls, then the sea flood-filled from a point known to be at sea
        mask = Image.new("L", img.size, 0); md = ImageDraw.Draw(mask)
        for w in ways(lambda t: t.get("natural") == "coastline"): md.line(G(w["geometry"]), fill=255, width=2 * S)
        ImageDraw.floodfill(mask, (X(sea[1]), Y(sea[0])), 128)
        water = mask.point(lambda v: 255 if v >= 128 else 0)
        img.paste(Image.new("RGBA", img.size, hexc(C_SEA)), (0, 0), water)
        d = ImageDraw.Draw(img)
    for r in ne_shapes(ne, "ne_10m_lakes", box): d.polygon(P(r), fill=hexc(C_SEA))
    over = Image.new("RGBA", img.size, (0, 0, 0, 0)); od = ImageDraw.Draw(over)
    for r in ne_shapes(ne, "ne_10m_urban_areas", box): od.polygon(P(r), fill=(148, 178, 186, 33))
    img = Image.alpha_composite(img, over); d = ImageDraw.Draw(img)
    if sea:
        for w in ways(lambda t: t.get("natural") == "coastline"): d.line(G(w["geometry"]), fill=hexc("#3b7f9c"), width=int(1.2 * S), joint="curve")
    for w in ways(lambda t: t.get("highway") == "trunk"): d.line(G(w["geometry"]), fill=hexc("#2b444d"), width=int(1.1 * S), joint="curve")
    for w in ways(lambda t: t.get("highway") == "motorway"): d.line(G(w["geometry"]), fill=hexc("#34515b"), width=int(1.4 * S), joint="curve")
    # rivers: only the long named ones (the small ones clutter the map)
    def km(g): return sum(math.hypot((b["lat"] - a["lat"]) * 111.32, (b["lon"] - a["lon"]) * km_lon) for a, b in zip(g, g[1:]))
    river_km = {}
    for w in ways(lambda t: t.get("waterway") == "river" and t.get("name")):
        river_km[w["tags"]["name"]] = river_km.get(w["tags"]["name"], 0) + km(w["geometry"])
    for w in ways(lambda t: t.get("waterway") == "river" and river_km.get(t.get("name"), 0) >= 15):
        d.line(G(w["geometry"]), fill=hexc("#4fa3c7"), width=int((1.4 if sea else 2.0) * S), joint="curve")
    for l in ne_shapes(ne, "ne_10m_admin_0_boundary_lines_land", box): dashed(d, P(l), hexc("#6d8c97"), 1, 2, 4)
    for w in ways(lambda t: t.get("aeroway") == "runway"):   # airliner runways only (1.5 km and more)
        if km(w["geometry"]) >= 1.5: d.line(G(w["geometry"]), fill=hexc("#e6eff1"), width=int(2.2 * S))

    taken = [(4 * S, (H - 46) * S, 300 * S, H * S), ((W - 100) * S, (H - 18) * S, W * S, H * S)]   # legend, scale, credit
    def free(b): return all(b[2] < t[0] or b[0] > t[2] or b[3] < t[1] or b[1] > t[3] for t in taken) and \
        b[0] > 2 * S and b[2] < (W - 2) * S and b[1] > 2 * S and b[3] < (H - 2) * S
    def place(s, x, y, f, anchor):
        x0, y0, x1, y1 = d.textbbox((x, y), s, font=f, anchor=anchor)
        b = (x0 - 3 * S, y0 - 2 * S, x1 + 3 * S, y1 + 2 * S)
        if not free(b): return False
        taken.append(b); return True

    # main airport: 10 and 20 NM rings; every airport: its code in yellow
    mx, my = X(apts[0][3]), Y(apts[0][2])
    for nm in (10, 20):
        r = nm * 1.852 * km_px
        d.ellipse([mx - r, my - r, mx + r, my + r], outline=(180, 205, 212, 34), width=S)
        text(d, f"{nm} NM", mx, my - r, font(F_SEMI, 9), hexc("#7f9aa3"), "mm")
    for code, _, lat, lon in apts:
        x, y = X(lon), Y(lat) + 13 * S
        if place(code, x, y, font(F_BOLD, 11), "mm"): text(d, code, x, y, font(F_BOLD, 11), hexc("#ffc857"), "mm")
    for s, lon, lat, kind in labels:
        f, col, sp, halo = {"country": (font(F_BOLD, 11), (205, 222, 227, 107), 3, False),
                            "sea": (font(F_SEMI, 10), hexc("#5f97ad"), 2.5, False),
                            "water": (font(F_SEMI, 10), hexc("#7fbcd4"), 2, True),
                            "relief": (font(F_SEMI, 10), hexc("#8fb3a4"), 2.5, True)}[kind]
        anchor = "lm" if kind == "sea" else "mm"
        text(d, s, X(lon), Y(lat), f, col, anchor, halo=halo, spacing=sp)
        x0, y0, x1, y1 = d.textbbox((X(lon), Y(lat)), s, font=f, anchor=anchor)
        taken.append((x0 - 20 * S, y0, x1 + 20 * S, y1))

    # towns: the reference city first, then by importance
    rank = {"city": 0, "borough": 1, "town": 2}
    towns = []
    for e in osm:
        t = e.get("tags", {})
        if e["type"] != "node" or "place" not in t: continue
        name = latin1(t.get("name:fr") or t.get("name:en") or t.get("name"))
        if not name or not (lat0 < e["lat"] < lat1 and lon0 < e["lon"] < lon1): continue
        try: pop = int(str(t.get("population", "0")).replace(" ", "").replace(",", "").split(".")[0])
        except ValueError: pop = 0
        if math.hypot((e["lat"] - ref[1]) * 111.32, (e["lon"] - ref[2]) * km_lon) < 4: continue
        if {fold(t.get(k, "")) for k in ("name", "name:en", "name:fr")} & {fold(ref[0]), fold(title)}: continue   # the reference city itself
        towns.append((rank[t["place"]], -pop, name, e["lon"], e["lat"]))
    towns.sort()
    shown = [(ref[0], ref[2], ref[1], 1)] + [(n, lon, lat, 2 if r < 2 or -p > 100000 else 3) for r, p, n, lon, lat in towns]
    count = 0
    for s, lon, lat, tier in shown:
        if count >= 16: break
        x, y = X(lon), Y(lat); r = {1: 3.6, 2: 2.6, 3: 2}[tier] * S
        f = font(F_BOLD, 15) if tier == 1 else font(F_SEMI, 12) if tier == 2 else font(F_MED, 11)
        col = hexc("#ffffff" if tier == 1 else "#d5e2e6" if tier == 2 else "#aec2c9")
        for side in ("r", "l"):
            tx = x + r + 4 * S if side == "r" else x - r - 4 * S
            anchor = "lm" if side == "r" else "rm"
            if place(s, tx, y, f, anchor) or tier == 1:
                d.ellipse([x - r, y - r, x + r, y + r], fill=hexc("#ffffff" if tier == 1 else "#c5d6db"))
                text(d, s, tx, y, f, col, anchor); count += 1
                break
    legend(d, km_px)
    text(d, "© OpenStreetMap", (W - 6) * S, (H - 8) * S, font(F_MED, 9), hexc("#7f9aa3"), "rm")   # ODbL attribution
    return img.resize((W, H), Image.LANCZOS).convert("RGB")


def legend(d, km_px):
    stops = [(0, (255, 157, 92)), (5000, (255, 209, 102)), (15000, (127, 227, 161)), (30000, (108, 201, 255)), (42000, (185, 167, 255))]
    lx, ly, lw = 10 * S, (H - 34) * S, 172 * S
    d.rectangle([lx - 4 * S, ly - 8 * S, lx + lw + 24 * S, ly + 30 * S], fill=(15, 28, 34, 210))
    for i in range(lw):
        a = i / lw * 42000
        for j in range(1, len(stops)):
            if a <= stops[j][0]:
                (p, c), (q, e) = stops[j - 1], stops[j]; t = (a - p) / (q - p)
                d.line([(lx + i, ly - 3 * S), (lx + i, ly + 3 * S)], fill=tuple(round(c[k] + (e[k] - c[k]) * t) for k in range(3)))
                break
    for a, s in ((0, "0"), (10000, "10k"), (20000, "20k"), (30000, "30k"), (40000, "40k ft")):
        x = lx + a / 42000 * lw
        d.rectangle([x, ly + 3 * S, x + S, ly + 6 * S], fill=hexc("#aec2c9"))
        text(d, s, x + (8 * S if a == 40000 else 0), ly + 13 * S, font(F_SEMI, 9), hexc("#aec2c9"), "lm" if a == 0 else "mm", halo=False)
    k10 = 10 * km_px; sx, sy = lx + lw + 36 * S, (H - 14) * S
    d.rectangle([sx - 5 * S, sy - 20 * S, sx + k10 + 5 * S, sy + 7 * S], fill=(15, 28, 34, 210))
    d.line([(sx, sy - 4 * S), (sx, sy), (sx + k10, sy), (sx + k10, sy - 4 * S)], fill=hexc("#d5e2e6"), width=int(1.5 * S))
    text(d, "10 km", sx + k10 / 2, sy - 11 * S, font(F_SEMI, 10), hexc("#d5e2e6"), "mm", halo=False)


def cstr(s): return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ne, cache = Path(sys.argv[1]), Path(sys.argv[2])
    cache.mkdir(parents=True, exist_ok=True)
    prev = cache / "previews"; prev.mkdir(exist_ok=True)   # PNG previews, outside the repo
    blobs, rows = [], []
    for z in ZONES:
        zid, title, ref, centre, apts, _, _ = z
        if zid == "bale":   # hand-made map, original extent
            img = ga.basemap(); lat0, lat1, lon0, lon1 = ga.LAT0, ga.LAT1, ga.LON0, ga.LON1
        else:
            img = draw_zone(z, ne, cache); lat0, lat1, lon0, lon1 = extent(centre)
        img.save(prev / f"{zid}.png")
        raw = bytes(ga.rgb565(img)); comp = zlib.compress(raw, 9)
        print(f"{zid}: {len(comp) // 1024} KB")
        rows_b = [", ".join(f"0x{b:02x}" for b in comp[i:i + 24]) for i in range(0, len(comp), 24)]
        blobs.append(f"static const uint8_t map_{zid}[] = {{\n    " + ",\n    ".join(rows_b) + "\n};\n")
        km_lon = 111.32 * math.cos(math.radians((lat0 + lat1) / 2))
        ap = ", ".join(f"{{\"{c}\", {cstr(city)}, {la:.4f}f, {lo:.4f}f}}" for c, city, la, lo in apts)
        rows.append(f"    {{{cstr(title)}, {cstr(ref[0])}, {lat0:.4f}f, {lat1:.4f}f, {lon0:.4f}f, {lon1:.4f}f, "
                    f"{ref[1]:.4f}f, {ref[2]:.4f}f, {km_lon:.2f}f, {{{ap}}}, {len(apts)}, map_{zid}, sizeof map_{zid}}},")
    (OUT / "zones.c").write_text(
        "/* Selectable places and their base maps (800x440 RGB565, zlib): generated by tools/gen_maps.py, do not edit */\n"
        "#include \"zones.h\"\n\n" + "\n".join(blobs) + "\nconst zone_t zones[] = {\n" + "\n".join(rows) +
        "\n};\nconst int zones_n = sizeof zones / sizeof zones[0];\n", encoding="utf8")
    print("ok", (OUT / "zones.c").stat().st_size)


if __name__ == "__main__":
    main()

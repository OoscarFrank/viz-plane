#!/usr/bin/env python3
"""Generates the base map (800x440, RGB565) and the aircraft icons for LVGL 8.

Geographic data comes from ../radar.py (the GEO object of the simulator page).
Map labels stay in French, like the rest of the on-screen text.
Usage: uv run --with pillow python tools/gen_assets.py
"""
import json, math, re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "components" / "radar_app" / "assets"
SRC = ROOT.parent.parent / "radar.py"

# Map extent: same as the simulator and app_internal.h
LON0, LON1, LAT0, LAT1 = 6.93, 8.01, 47.47, 47.87
W, H = 800, 440
S = 3  # supersampling for anti-aliasing
KM_LON = 111.32 * math.cos(math.radians(47.67))

GEO = json.loads(re.search(r"const GEO=(\{.*?\});", SRC.read_text(encoding="utf8"), re.S).group(1))


def X(lon): return (lon - LON0) / (LON1 - LON0) * W * S
def Y(lat): return (LAT1 - lat) / (LAT1 - LAT0) * H * S
def pts(ring): return [(X(p[0]), Y(p[1])) for p in ring]
def hexc(h, a=255): h = h.lstrip("#"); return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (a,)


def font(name, px): return ImageFont.truetype(str(ROOT / name), px * S)
F_MED, F_SEMI, F_BOLD = "Montserrat-Medium.ttf", "Montserrat-SemiBold.ttf", "Montserrat-Bold.ttf"


def text(d, s, x, y, f, fill, anchor="lm", halo=True, spacing=0):
    if spacing:
        # letter-spaced text: place the letters one by one
        widths = [f.getlength(c) + spacing * S for c in s]
        total = sum(widths) - spacing * S
        cx = x - total / 2 if anchor[0] == "m" else x
        for c, w in zip(s, widths):
            if halo: d.text((cx, y), c, font=f, fill=hexc("#0f1c22", 200), anchor="l" + anchor[1], stroke_width=2 * S, stroke_fill=hexc("#0f1c22", 200))
            d.text((cx, y), c, font=f, fill=fill, anchor="l" + anchor[1]); cx += w
        return
    if halo:
        d.text((x, y), s, font=f, fill=fill, anchor=anchor, stroke_width=int(1.5 * S), stroke_fill=hexc("#0f1c22", 220))
    else:
        d.text((x, y), s, font=f, fill=fill, anchor=anchor)


def dashed(d, line, color, width, on, off):
    acc, draw_on = 0.0, True
    for (x0, y0), (x1, y1) in zip(line, line[1:]):
        seg = math.hypot(x1 - x0, y1 - y0); t = 0.0
        while t < seg:
            step = min((on if draw_on else off) * S - acc, seg - t)
            if draw_on:
                a, b = t / seg, (t + step) / seg
                d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)], fill=color, width=int(width * S))
            t += step; acc += step
            if acc >= (on if draw_on else off) * S - 1e-6: acc, draw_on = 0.0, not draw_on


def basemap():
    img = Image.new("RGBA", (W * S, H * S), hexc("#14252c"))
    d = ImageDraw.Draw(img)
    for g, c in ((GEO["fr"], "#14252c"), (GEO["de"], "#172a30"), (GEO["ch"], "#1a2d33")):
        for r in g: d.polygon(pts(r), fill=hexc(c))
    # relief: blurred halos
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0)); gd = ImageDraw.Draw(glow)
    for lon, lat, r, a in ((6.95, 47.88, 250, 40), (7.0, 47.70, 140, 22), (7.92, 47.80, 300, 40), (7.35, 47.40, 170, 34), (7.72, 47.40, 190, 36)):
        gd.ellipse([X(lon) - r * S * .6, Y(lat) - r * S * .6, X(lon) + r * S * .6, Y(lat) + r * S * .6], fill=(136, 170, 150, a))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(60 * S)))
    over = Image.new("RGBA", img.size, (0, 0, 0, 0)); od = ImageDraw.Draw(over)
    for r in GEO["urban"]: od.polygon(pts(r), fill=(148, 178, 186, 33))
    img = Image.alpha_composite(img, over); d = ImageDraw.Draw(img)
    for l in GEO["roads"]: d.line(pts(l), fill=hexc("#34515b"), width=int(1.4 * S), joint="curve")
    for l in GEO["ill"]: d.line(pts(l), fill=hexc("#3b7f9c"), width=int(1.2 * S), joint="curve")
    for g in (GEO["fr"], GEO["ch"]):
        for r in g: dashed(d, pts(r) + pts(r)[:1], hexc("#6d8c97"), 1, 2, 4)
    for l in GEO["rhine"]: d.line(pts(l), fill=hexc("#4fa3c7"), width=int(2.4 * S), joint="curve")
    # airport: 10 and 20 NM rings, approach path, runway
    bx, by = X(7.529), Y(47.590)
    for nm in (10, 20):
        r = nm * 1.852 / KM_LON / (LON1 - LON0) * W * S
        d.ellipse([bx - r, by - r, bx + r, by + r], outline=(180, 205, 212, 34), width=S)
        text(d, f"{nm} NM", bx, by - r, font(F_SEMI, 9), hexc("#7f9aa3"), "mm")
    dashed(d, [(X(7.518), Y(47.606)), (X(7.394), Y(47.786))], (255, 200, 87, 72), 1, 5, 5)
    d.line([(X(7.518), Y(47.606)), (X(7.540), Y(47.574))], fill=hexc("#e6eff1"), width=3 * S)
    text(d, "BSL · MLH", bx - 9 * S, by + 11 * S, font(F_BOLD, 11), hexc("#ffc857"), "rm")
    hx, hy = X(7.432), Y(47.738)
    d.ellipse([hx - 3 * S, hy - 3 * S, hx + 3 * S, hy + 3 * S], outline=hexc("#9fb7bf"), width=int(1.2 * S))
    text(d, "LFGB", hx + 6 * S, hy, font(F_SEMI, 9), hexc("#9fb7bf"))
    for s, lon, lat in (("VOSGES", 7.03, 47.845), ("SUNDGAU", 7.20, 47.545), ("JURA", 7.50, 47.487), ("FORÊT-NOIRE", 7.86, 47.76)):
        text(d, s, X(lon), Y(lat), font(F_SEMI, 10), hexc("#8fb3a4"), "mm", spacing=2.5)
    text(d, "Forêt de la Hardt", X(7.47), Y(47.80), font(F_MED, 10), hexc("#7fa58f"), "mm")
    for s, lon, lat in (("FRANCE", 7.09, 47.70), ("ALLEMAGNE", 7.77, 47.685), ("SUISSE", 7.40, 47.515)):
        text(d, s, X(lon), Y(lat), font(F_BOLD, 11), (205, 222, 227, 107), "mm", halo=False, spacing=3)
    cities = [("Mulhouse", 7.3359, 47.7508, 1, "r"), ("Bâle", 7.5886, 47.5596, 1, "l"), ("Saint-Louis", 7.5600, 47.5900, 2, "r"),
              ("Lörrach", 7.6610, 47.6150, 2, "r"), ("Altkirch", 7.2397, 47.6236, 2, "l"), ("Thann", 7.1030, 47.8070, 2, "l"),
              ("Cernay", 7.1760, 47.8090, 3, "r"), ("Ensisheim", 7.3530, 47.8660, 3, "r"), ("Sierentz", 7.4550, 47.6600, 3, "l"),
              ("Neuenburg", 7.5600, 47.8130, 3, "l"), ("Müllheim", 7.6300, 47.8080, 2, "r"), ("Schopfheim", 7.8220, 47.6500, 3, "r"),
              ("Liestal", 7.7340, 47.4840, 2, "r"), ("Rheinfelden", 7.7940, 47.5540, 3, "r"), ("Dannemarie", 7.1210, 47.6300, 3, "l"),
              ("Masevaux", 6.9960, 47.7740, 3, "r")]
    for s, lon, lat, t, side in cities:
        x, y = X(lon), Y(lat); r = {1: 3.6, 2: 2.6, 3: 2}[t] * S
        d.ellipse([x - r, y - r, x + r, y + r], fill=hexc("#ffffff" if t == 1 else "#c5d6db"))
        f = font(F_BOLD, 15) if t == 1 else font(F_SEMI, 12) if t == 2 else font(F_MED, 11)
        col = hexc("#ffffff" if t == 1 else "#d5e2e6" if t == 2 else "#aec2c9")
        text(d, s, x - r - 4 * S if side == "l" else x + r + 4 * S, y, f, col, "rm" if side == "l" else "lm")
    # altitude legend + 10 km scale bar
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
    k10 = 10 / KM_LON / (LON1 - LON0) * W * S; sx, sy = lx + lw + 36 * S, (H - 14) * S
    d.rectangle([sx - 5 * S, sy - 20 * S, sx + k10 + 5 * S, sy + 7 * S], fill=(15, 28, 34, 210))
    d.line([(sx, sy - 4 * S), (sx, sy), (sx + k10, sy), (sx + k10, sy - 4 * S)], fill=hexc("#d5e2e6"), width=int(1.5 * S))
    text(d, "10 km", sx + k10 / 2, sy - 11 * S, font(F_SEMI, 10), hexc("#d5e2e6"), "mm", halo=False)
    return img.resize((W, H), Image.LANCZOS).convert("RGB")


def c_array(name, data, w, h, cf, comment):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    body = ",\n    ".join(rows)
    return (f"/* {comment}: generated by tools/gen_assets.py, do not edit */\n#include \"lvgl.h\"\n\n"
            f"static const LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] = {{\n    {body}\n}};\n\n"
            f"const lv_img_dsc_t {name} = {{\n    .header.cf = {cf},\n    .header.always_zero = 0,\n"
            f"    .header.w = {w},\n    .header.h = {h},\n    .data_size = {len(data)},\n    .data = {name}_map,\n}};\n")


def rgb565(img):
    out = bytearray()
    for r, g, b in img.get_flattened_data():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v & 0xFF, v >> 8))          # LV_COLOR_16_SWAP = 0
    return bytes(out)


# Top-view silhouettes, nose up: right half only, mirrored to the left (as in the simulator)
JET = [(0, -13), (.8, -12), (1.5, -10), (1.7, -7), (1.7, -3.2), (12, 3.6), (12, 5.6), (1.7, 2.8), (1.4, 8), (5, 11), (5, 12.6), (.7, 11.4), (0, 13)]
LIGHT = [(0, -10), (1, -9), (1.4, -6), (1.5, -4.5), (11.5, -3.6), (11.5, -.8), (1.3, -.2), (.8, 6.6), (4.2, 7.2), (4.2, 9.2), (.5, 9.4), (0, 10)]


def mirrored(half): return half + [(-x, y) for x, y in reversed(half[1:-1])]


def icon(kind, outline, size=32):
    k = 8; c = size * k / 2; sc = k * size / 30
    im = Image.new("L", (size * k, size * k), 0); d = ImageDraw.Draw(im)
    if kind == "heli":
        d.ellipse([c - 3.2 * sc, c - 7 * sc, c + 3.2 * sc, c + 4 * sc], fill=255)
        d.rectangle([c - .8 * sc, c + 3 * sc, c + .8 * sc, c + 11 * sc], fill=255)
        d.rectangle([c - 3 * sc, c + 11 * sc, c + 3 * sc, c + 13 * sc], fill=255)
        for a, b in (((-9, -8.5), (9, 5.5)), ((9, -8.5), (-9, 5.5))):
            d.line([(c + a[0] * sc, c + a[1] * sc), (c + b[0] * sc, c + b[1] * sc)], fill=255, width=int(1.6 * sc))
    else:
        d.polygon([(c + x * sc, c + y * sc) for x, y in mirrored(JET if kind == "jet" else LIGHT)], fill=255)
    if outline: im = im.filter(ImageFilter.MaxFilter(int(1.3 * sc) | 1))
    return im.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bg = basemap(); bg.save(ROOT / "map_bg_preview.png")
    (OUT / "map_bg.c").write_text(c_array("img_map_bg", rgb565(bg), W, H, "LV_IMG_CF_TRUE_COLOR", "Mulhouse-Basel base map 800x440 RGB565"))
    parts = []
    for kind in ("jet", "light", "heli"):
        for ol in (False, True):
            nm = f"img_{kind}{'_ol' if ol else ''}"
            # RGB565 + alpha: LVGL 8 cannot rotate plain A8 images. White, recoloured at draw time.
            data = bytearray()
            for a in icon(kind, ol).tobytes(): data += bytes((0xFF, 0xFF, a))
            parts.append(c_array(nm, bytes(data), 32, 32, "LV_IMG_CF_TRUE_COLOR_ALPHA", f"{kind} icon 32x32 RGB565+A8"))
    (OUT / "icons.c").write_text("\n".join(p if i == 0 else p.split("\n", 2)[2] for i, p in enumerate(parts)))
    print("ok", (OUT / "map_bg.c").stat().st_size, (OUT / "icons.c").stat().st_size)


if __name__ == "__main__":
    main()

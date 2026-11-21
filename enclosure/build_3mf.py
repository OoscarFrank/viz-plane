#!/usr/bin/env python3
"""Packs the 3 parts into a Bambu Studio 3MF project.

Plate 1 "Black":  stand, filament 1
Plate 2 "Orange": frame, filament 2
Plate 3 "Black":  shell, filament 1
The stand and the shell carry fast print settings (0.28 mm layers, 2 walls, 10% infill).
Usage: python3 build_3mf.py <folder containing frame.3mf shell.3mf stand.3mf> [part ...]
With part names, only those parts are packed, into <first part>_bambu.3mf.
"""
import re, sys, zipfile
from pathlib import Path

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
BED = 256.0                     # X1 / P1 / A1
STRIDE = BED * 1.2              # spacing between plates in Bambu Studio

# name, filament, plate, centre (x, y) on the plate, in mm
PARTS = [("stand", 1, 1, (128, 128)), ("frame", 2, 2, (128, 128)), ("shell", 1, 3, (128, 128))]
FAST = {"layer_height": "0.28", "wall_loops": "2", "sparse_infill_density": "10%",
        "sparse_infill_pattern": "gyroid", "top_shell_layers": "3", "bottom_shell_layers": "3"}
SETTINGS = {"stand": FAST, "shell": FAST}
ONLY = sys.argv[2:]
if ONLY:
    PARTS = [(n, f, 1, c) for n, f, _, c in PARTS if n in ONLY]
OUT = Path(__file__).with_name(f"{ONLY[0]}_bambu.3mf" if ONLY else "radar_enclosure_bambu.3mf")


def mesh(name):
    xml = zipfile.ZipFile(SRC / f"{name}.3mf").read("3D/3dmodel.model").decode()
    v = [tuple(map(float, m)) for m in re.findall(r'<vertex x="([^"]+)" y="([^"]+)" z="([^"]+)"', xml)]
    t = re.findall(r'<triangle v1="(\d+)" v2="(\d+)" v3="(\d+)"', xml)
    lo = [min(p[i] for p in v) for i in range(3)]
    hi = [max(p[i] for p in v) for i in range(3)]
    c = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]]          # centred in X/Y, resting on Z=0
    return [(x - c[0], y - c[1], z - c[2]) for x, y, z in v], t, [hi[i] - lo[i] for i in range(3)]


objs, items, cfg_objs, plates = [], [], [], {}
for i, (name, fil, plate, (cx, cy)) in enumerate(PARTS, start=1):
    v, t, size = mesh(name)
    assert size[0] < BED - 4 and size[1] < BED - 4, name
    vs = "".join(f'<vertex x="{x:.4f}" y="{y:.4f}" z="{z:.4f}"/>' for x, y, z in v)
    ts = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in t)
    objs.append(f'<object id="{i}" name="{name}" type="model"><mesh><vertices>{vs}</vertices><triangles>{ts}</triangles></mesh></object>')
    ox = (plate - 1) * STRIDE
    items.append(f'<item objectid="{i}" transform="1 0 0 0 1 0 0 0 1 {cx + ox:.3f} {cy:.3f} 0" printable="1"/>')
    cfg_objs.append(
        f'<object id="{i}"><metadata key="name" value="{name}"/><metadata key="extruder" value="{fil}"/>'
        + "".join(f'<metadata key="{k}" value="{v}"/>' for k, v in SETTINGS.get(name, {}).items()) +
        f'<part id="1" subtype="normal_part"><metadata key="name" value="{name}"/>'
        f'<metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/><metadata key="extruder" value="{fil}"/></part></object>')
    plates.setdefault(plate, []).append(i)
    print(f"{name}: {size[0]:.1f} x {size[1]:.1f} x {size[2]:.1f} mm, plate {plate}, filament {fil}")

model = ('<?xml version="1.0" encoding="UTF-8"?>\n'
         '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
         'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021">'
         '<metadata name="Application">BambuStudio-02.09.01.70</metadata>'
         '<metadata name="BambuStudio:3mfVersion">1</metadata>'
         '<metadata name="Title">Mulhouse-Basel radar: 7-inch enclosure</metadata>'
         f'<resources>{"".join(objs)}</resources><build>{"".join(items)}</build></model>')

names = {1: "Black: stand", 2: "Orange: frame", 3: "Black: shell"}
plate_xml = "".join(
    f'<plate><metadata key="plater_id" value="{p}"/><metadata key="plater_name" value="{names[p]}"/>'
    f'<metadata key="locked" value="false"/>'
    + "".join(f'<model_instance><metadata key="object_id" value="{o}"/><metadata key="instance_id" value="0"/>'
              f'<metadata key="identify_id" value="{100 + o}"/></model_instance>' for o in ids)
    + "</plate>" for p, ids in plates.items())
config = f'<?xml version="1.0" encoding="UTF-8"?>\n<config>{"".join(cfg_objs)}{plate_xml}</config>\n'

ctypes = ('<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
          '<Default Extension="config" ContentType="text/xml"/></Types>')
rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
        '</Relationships>')

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", ctypes)
    z.writestr("_rels/.rels", rels)
    z.writestr("3D/3dmodel.model", model)
    z.writestr("Metadata/model_settings.config", config)
print("written:", OUT)

# Radar enclosure (Waveshare ESP32-S3-Touch-LCD-7)

Three parts that print without supports. Parametric source: `radar_enclosure.scad` (OpenSCAD).

| Part | Colour | Size (mm) | Print orientation |
|---|---|---|---|
| `frame.stl` | orange | 202 × 120 × 7 | front face on the bed |
| `shell.stl` | black | 198 × 116 × 24 | back on the bed |
| `stand.stl` | black | 198 × 93 × 103 | base on the bed |

Bed of at least 205 × 125 mm. PLA or PETG, 0.2 mm layers, 3 walls, 15% infill.

`radar_enclosure_bambu.3mf` is a Bambu Studio project with two plates: plate 1 (shell + stand) on filament 1, plate 2 (frame) on filament 2. It has not been opened in Bambu Studio yet; if it misbehaves, import the three STL files instead. `viewer.html` is a 3D viewer of the assembly.

## Hardware

Everything comes from the kit of 340 M3 stainless button-head screws (ISO 7380), 5 to 20 mm.

- 4 × M3 × 16: screen → shell (into the screen's brass inserts).
- 4 × M3 × 12: shell → stand (straight into the plastic). Their 1.65 mm heads fit under the board.

## Assembly

1. Screw the shell onto the stand: 4 × M3 × 12, from inside the shell.
2. Route the USB-C cable through the slot in the right side wall and plug it into the board.
3. Drop the screen into the shell, glass facing out.
4. Screw the screen in from the back: 4 × M3 × 16. The top screws go through the holes in the back of the stand, the bottom ones through the holes underneath.
5. Push the orange frame on. Add a dot of glue or double-sided tape if it moves.

## Check on the real board before printing

The dimensions come from the Waveshare drawing and 3D model, not from a measured board.

- Spacing of the 4 M3 holes: 126.2 × 65.65 mm.
- USB-C ports: right edge seen from the front, 51.8 and 66.9 mm from the top of the glass. Pick the port with `usb_port`.
- Depth of the screen's M3 inserts: 2 mm of thread engaged by default (`screw_engage`).
- Tallest component: 18.4 mm behind the glass (`COMP_Z`).

To regenerate the STL files after a change: `openscad -D 'part="shell"' -o shell.stl radar_enclosure.scad` (same for `frame` and `stand`).

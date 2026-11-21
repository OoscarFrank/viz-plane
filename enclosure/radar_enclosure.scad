// Wedge enclosure for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, touch)
// Three parts that print without supports: frame (front), shell, stand.
//
// "Enclosure" frame: X to the right, Y up, Z toward the viewer.
// Z = 0: front face of the glass. Screen dimensions come from the Waveshare
// drawing and 3D model (ESP32-S3-Touch-LCD-7in0_Drawing.zip): check them on
// the real board before printing.
//
// Render: part = "assembly" | "exploded" | "frame" | "shell" | "stand"

part = "assembly";

/* [Screen: Waveshare dimensions] */
GW = 192.96;   // glass, width
GH = 110.76;   // glass, height
GR = 7.80;     // glass, corner radius
// "Front" frame: x from the left edge of the glass, y' from the top
SCREEN_HOLES = [[31.85, 23.53], [158.05, 23.53], [31.85, 89.18], [158.05, 89.18]]; // 4 x M3 (brass inserts)
BLOCK_Z  = 8.5;    // face of the M3 mounting blocks behind the glass
PCB      = [45.51, 29.66, 151.52, 102.68];  // x0, y0', x1, y1'
PCB_Z    = 8.5;    // screen-side face of the board
COMP_Z   = 18.4;   // tallest component (terminal blocks)
USB_Y    = [51.84, 66.94];  // centres of the 2 USB-C ports (right edge of the board)
USB_Z    = 11.27;  // depth of the port centres
BUTTONS_X = [92.3, 103.4];  // BOOT/RESET buttons on the bottom edge of the board
BUTTON_Z = 10.2;

/* [Settings] */
tilt      = 18;    // backward tilt (°)
back_angle = 60;   // slope of the stand's back (° from the table)
usb_port  = 0;     // 0 = upper port, 1 = lower port
gap       = 0.3;   // glass / shell clearance
wall      = 2.4;
back      = 2.4;   // back panel thickness
inner_clear = 2.6; // clearance behind the components
lip       = 3.0;   // frame overlap on the black glass border
plate     = 2.0;   // frame face thickness
skirt     = 5.0;   // frame skirt depth
skirt_t   = 1.6;
skirt_gap = 0.15;
screw_len = 16;    // screen screws: M3 x 16 button head ISO 7380
screw_engage = 2.0;// thread engaged in the screen insert
cable_w   = 5.6;   // cable slot width
$fn = 64;

// ---------- derived ----------
IN_BACK  = -(COMP_Z + inner_clear);      // inner face of the back panel
OUT_BACK = IN_BACK - back;               // outer face of the back panel
WALL_TOP = 0.2;                          // the glass sits 0.2 mm below the wall top
BOX_W = GW + 2 * (gap + wall);
BOX_H = GH + 2 * (gap + wall);
BOX_R = GR + gap + wall;
BEZ_W = BOX_W + 2 * (skirt_gap + skirt_t);
BEZ_H = BOX_H + 2 * (skirt_gap + skirt_t);
BEZ_R = BOX_R + skirt_gap + skirt_t;
BOSS_END = -(BLOCK_Z + 0.2);
// front coordinates -> enclosure frame
function P(x, yp) = [x - GW / 2, GH / 2 - yp];
STAND_SCREWS = [[70, 40], [123, 40], [70, 95], [123, 95]];  // shell -> stand screws (heads inside)

module rrect(w, h, r) { offset(r) square([w - 2 * r, h - 2 * r], center = true); }
module slab(w, h, r, z0, z1) { translate([0, 0, z0]) linear_extrude(z1 - z0) rrect(w, h, r); }

// ---------- shell (black) ----------
module shell() {
  cb = (back + (IN_BACK < BOSS_END ? BOSS_END - IN_BACK : 0)) - (screw_len - screw_engage);
  difference() {
    union() {
      difference() {
        slab(BOX_W, BOX_H, BOX_R, OUT_BACK, WALL_TOP);
        slab(GW + 2 * gap, GH + 2 * gap, GR + gap, IN_BACK, WALL_TOP + 1);
      }
      for (h = SCREEN_HOLES) translate([each P(h[0], h[1]), IN_BACK - 0.01]) cylinder(d = 8, h = BOSS_END - IN_BACK);
    }
    // screen screws from the back + counterbore that sets the engaged length
    for (h = SCREEN_HOLES) translate([each P(h[0], h[1]), OUT_BACK - 1]) {
      cylinder(d = 3.4, h = 40);
      cylinder(d = 6.5, h = 1 + max(cb, 0));
    }
    // former shell -> stand screws (unused since the one-piece easel stand)
    for (s = STAND_SCREWS) translate([each P(s[0], s[1]), OUT_BACK - 1]) cylinder(d = 3.4, h = 10);
    // USB-C cable slot in the right side wall (open toward the front)
    translate([GW / 2 - 1, P(0, USB_Y[usb_port])[1], 0]) rotate([0, 90, 0]) hull() {
      translate([USB_Z, 0, 0]) cylinder(d = cable_w, h = wall + gap + 3);
      translate([-2, 0, 0]) cylinder(d = cable_w, h = wall + gap + 3);
    }
    // holes to press BOOT / RESET with a pin
    for (bx = BUTTONS_X) translate([P(bx, 0)[0], -GH / 2 + 1, -BUTTON_Z]) rotate([90, 0, 0]) cylinder(d = 2.5, h = wall + gap + 3);
    // top and bottom vents (vertical slots, no overhang)
    for (i = [-3 : 3]) for (sy = [1, -1]) {
      vx = i * 14 + (sy < 0 ? 30 : 0);
      if (sy > 0 || abs(vx - P(BUTTONS_X[0], 0)[0]) > 8 && abs(vx - P(BUTTONS_X[1], 0)[0]) > 8)
        translate([vx, sy * (GH / 2 + gap + wall / 2), (IN_BACK + 4 + -6) / 2]) cube([2, wall + 2, -IN_BACK - 10], center = true);
    }
  }
}

// ---------- frame (orange) ----------
module frame() {
  difference() {
    union() {
      slab(BEZ_W, BEZ_H, BEZ_R, WALL_TOP, WALL_TOP + plate);
      difference() {
        slab(BEZ_W, BEZ_H, BEZ_R, WALL_TOP - skirt, WALL_TOP + 0.01);
        slab(BOX_W + 2 * skirt_gap, BOX_H + 2 * skirt_gap, BOX_R + skirt_gap, WALL_TOP - skirt - 1, WALL_TOP + 0.02);
      }
    }
    slab(GW - 2 * lip, GH - 2 * lip, GR - lip, WALL_TOP - 1, WALL_TOP + plate + 1);
    // no cable notch: the cable leaves the shell behind the skirt (USB_Z > skirt)
  }
}

// ---------- enclosure -> world (horizontal table) ----------
st = sin(tilt); ct = cos(tilt);
function W(p) = [p[0], p[1] * st - p[2] * ct, p[1] * ct + p[2] * st];
LOW = min([for (q = [[-BOX_H / 2, OUT_BACK], [-BEZ_H / 2, WALL_TOP + plate], [-BEZ_H / 2, WALL_TOP - skirt]]) q[0] * ct + q[1] * st]);
module to_world() { translate([0, 0, -LOW]) multmatrix([[1, 0, 0, 0], [0, st, -ct, 0], [0, ct, st, 0], [0, 0, 0, 1]]) children(); }

// ---------- easel stand (black): one piece, back plate + leg ----------
// The plate covers the whole back of the shell. The 4 screen screws go through
// stand + shell into the screen inserts and hold all three together (the shell's
// STAND_SCREWS holes are unused). Prints in one piece without supports: shell-side
// face on the bed, the leg rising at ~64 degrees.
plate_t   = 5;     // back plate thickness
plate_screw_len = 20;  // screen screws through stand + shell: M3 x 20 button head
hinge_y   = 22;    // where the leg meets the plate (enclosure y)
foot_y    = 110;   // where the leg meets the table (world y)
leg_t     = 6;     // leg thickness
leg_w     = [70, 116];  // leg width at the plate and on the table (screwdriver clears it)
wedge_len = 12;    // wedge between plate and leg, down from the hinge
ZP = OUT_BACK - plate_t;                 // outer face of the plate
function TW(p) = W(p) + [0, 0, -LOW];    // enclosure -> world point
function yz(p) = [p[1], p[2]];
HW = yz(TW([0, hinge_y, ZP]));           // hinge, world (y, z)
FW = [foot_y, 0];
LU = (FW - HW) / norm(FW - HW);          // along the leg, toward the foot
LN = [-LU[1], LU[0]];                    // leg normal, away from the plate
PV = [st, ct];                           // up along the plate
PN = [ct, -st];                          // plate normal, toward the back
function cross2(a, b) = a[0] * b[1] - a[1] * b[0];
QW = HW - wedge_len * PV;                // bottom of the wedge on the plate
RW = QW + cross2(HW - QW, LU) / cross2(PN, LU) * PN;   // ... and on the leg
LEG_LEN = norm(FW - HW);
module from_world() multmatrix([[1, 0, 0, 0], [0, st, ct, 0], [0, -ct, st, 0], [0, 0, 0, 1]]) translate([0, 0, LOW]) children();
module behind_plate() to_world() translate([-500, -500, ZP - 1000]) cube([1000, 1000, 1000]);
module above_table() translate([-500, -500, 0]) cube([1000, 1000, 1000]);
// leg frame: x, s along the leg, w away from the plate (origin at the hinge)
module leg_frame() multmatrix([[1, 0, 0, 0], [0, LU[0], LN[0], HW[0]], [0, LU[1], LN[1], HW[1]], [0, 0, 0, 1]]) children();

module stand() {
  cb = (back + (IN_BACK < BOSS_END ? BOSS_END - IN_BACK : 0)) + plate_t - (plate_screw_len - screw_engage);
  intersection() {
    difference() {
      union() {
        to_world() hull() {
          slab(BOX_W, BOX_H, BOX_R, ZP + 1, OUT_BACK);
          slab(BOX_W - 2, BOX_H - 2, BOX_R - 1, ZP, ZP + 1);
        }
        intersection() {
          union() {
            leg_frame() hull() {
              translate([0, -10, 0]) linear_extrude(leg_t) rrect(leg_w[0], 4, 1.9);
              translate([0, LEG_LEN + 15, 0]) linear_extrude(leg_t) rrect(leg_w[1], 4, 1.9);
            }
            rotate([90, 0, 90]) linear_extrude(leg_w[0] - 6, center = true) polygon([HW, QW, RW]);
          }
          behind_plate();
        }
      }
      to_world() {
        // screen screws, heads flush in the back face
        for (h = SCREEN_HOLES) translate([each P(h[0], h[1]), ZP - 1]) {
          cylinder(d = 3.4, h = plate_t + 2);
          cylinder(d = 6.5, h = 1 + cb);
        }
      }
    }
    above_table();   // the bottom edge sits flat on the table
  }
}

// ---------- screen mock-up (display only) ----------
module screen() {
  color("#111") slab(GW, GH, GR, -1.5, 0);
  color("#16303a") translate([P(19.04, 0)[0], P(0, 99.76)[1], 0.01]) cube([154.88, 86.72, 0.05]);
  color("#9aa") translate([P(12.09, 0)[0], P(0, 102.82)[1], -5]) cube([165.72, 97.6, 3.5]);
  for (h = SCREEN_HOLES) color("#eee") translate([each P(h[0], h[1]), -BLOCK_Z]) translate([-7, -4, 0]) cube([14, 8, 3.5]);
  color("#1d5fa8") translate([P(PCB[0], 0)[0], P(0, PCB[3])[1], -PCB_Z - 1.6]) cube([PCB[2] - PCB[0], PCB[3] - PCB[1], 1.6]);
  color("#333") translate([P(PCB[0] + 10, 0)[0], P(0, PCB[3] - 5)[1], -COMP_Z]) cube([60, 50, COMP_Z - PCB_Z - 1.6]);
  // USB-C plug + cable
  uy = P(0, USB_Y[usb_port])[1];
  color("#c8c8c8") translate([P(PCB[2], 0)[0], uy - 4.5, -USB_Z - 3.5]) cube([26, 9, 7]);
  color("#e8662a") translate([P(PCB[2], 0)[0] + 26, uy, -USB_Z]) rotate([0, 90, 0]) cylinder(d = 4.5, h = 60);
}

ORANGE = "#ef6a1f"; GREY = "#cfd2d4";
if (part == "assembly") {
  to_world() { color(GREY) shell(); color(ORANGE) frame(); screen(); }
  color(GREY) stand();
} else if (part == "exploded") {
  to_world() { color(GREY) shell(); translate([0, 0, 45]) color(ORANGE) frame(); translate([0, 0, 22]) screen(); }
  translate([0, 70, 0]) color(GREY) stand();
} else if (part == "w_shell") { to_world() shell();          // assembled pose, for the 3D viewer
} else if (part == "w_frame") { to_world() frame();
} else if (part == "w_screen") { to_world() screen();
} else if (part == "w_stand") { stand();
} else if (part == "frame") {
  translate([0, 0, WALL_TOP + plate]) mirror([0, 0, 1]) frame();   // front face on the bed
} else if (part == "shell") {
  translate([0, 0, -OUT_BACK]) shell();                            // back on the bed
} else if (part == "stand") {
  translate([0, 0, OUT_BACK]) mirror([0, 0, 1]) from_world() stand();   // shell-side face on the bed
}

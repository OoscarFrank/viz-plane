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
    // shell -> stand screws: M3 x 12 button head ISO 7380 (1.65 mm head fits under the board)
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
    // skirt notch for the cable
    translate([BOX_W / 2 - 1, P(0, USB_Y[usb_port])[1] - cable_w / 2, WALL_TOP - skirt - 1]) cube([skirt_t + skirt_gap + 3, cable_w, skirt + 1]);
  }
}

// ---------- enclosure -> world (horizontal table) ----------
st = sin(tilt); ct = cos(tilt);
function W(p) = [p[0], p[1] * st - p[2] * ct, p[1] * ct + p[2] * st];
LOW = min([for (q = [[-BOX_H / 2, OUT_BACK], [-BEZ_H / 2, WALL_TOP + plate], [-BEZ_H / 2, WALL_TOP - skirt]]) q[0] * ct + q[1] * st]);
module to_world() { translate([0, 0, -LOW]) multmatrix([[1, 0, 0, 0], [0, st, -ct, 0], [0, ct, st, 0], [0, 0, 0, 1]]) children(); }

// ---------- wedge stand (black), built directly on the table ----------
A = W([0, -BOX_H / 2, OUT_BACK]) + [0, 0, -LOW];
B = W([0, BOX_H / 2 - 6, OUT_BACK]) + [0, 0, -LOW];
C = [0, B[1] + B[2] / tan(back_angle), 0];
module stand() {
  difference() {
    rotate([90, 0, 90]) translate([0, 0, -BOX_W / 2]) linear_extrude(BOX_W)
      offset(r = 1.5) offset(delta = -1.5) polygon([[A[1], A[2]], [B[1], B[2]], [C[1], C[2]]]);
    // screwdriver access to the 4 screen screws
    to_world() for (h = SCREEN_HOLES) translate([each P(h[0], h[1]), OUT_BACK + 0.5]) mirror([0, 0, 1]) cylinder(d = 9, h = 150);
    // 2.6 mm pilot holes for the shell -> stand screws (M3 straight into the plastic)
    to_world() for (s = STAND_SCREWS) translate([each P(s[0], s[1]), OUT_BACK + 0.5]) mirror([0, 0, 1]) cylinder(d = 2.6, h = 12.5);
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
} else if (part == "frame") {
  translate([0, 0, WALL_TOP + plate]) mirror([0, 0, 1]) frame();   // front face on the bed
} else if (part == "shell") {
  translate([0, 0, -OUT_BACK]) shell();                            // back on the bed
} else if (part == "stand") {
  stand();                                                         // base on the bed
}

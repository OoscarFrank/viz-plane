/* LVGL 8 radar UI: map, aircraft, flight card, list, Wi-Fi settings.
 * Same layout as the web simulator (radar.py), at 800x480. On-screen text stays in French. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "plat.h"
#include "radar_app.h"
#include "app_internal.h"

/* ---------- colours (the screen is always dark) ---------- */
#define C_BG     lv_color_hex(0x0f1c22)
#define C_PANEL  lv_color_hex(0x14252c)
#define C_RAISE  lv_color_hex(0x1d333c)
#define C_LINE   lv_color_hex(0x2c4650)
#define C_FG     lv_color_hex(0xe6eff1)
#define C_DIM    lv_color_hex(0x93aab2)
#define C_ACCENT lv_color_hex(0xffc857)
#define C_OK     lv_color_hex(0x6fdc9c)
#define C_ERR    lv_color_hex(0xff8a7a)
#define C_INK    lv_color_hex(0x14252c)
#define C_EMERG  lv_color_hex(0xff5a4a)
#define C_MIL    lv_color_hex(0xc3cf6e)
#define C_BIG    lv_color_hex(0xc9a2ff)

#define PANEL_W 276
#define TRAIL_N 24
#define PROF_N 60
#define KM_LAT 111.32f
#define KM_LON (zone->km_lon)

typedef struct {
    bool used;
    ac_t d;
    uint32_t t;              /* when the last position arrived */
    float off_lat, off_lon;  /* offset to absorb for smooth motion */
    float rlat, rlon;        /* displayed position */
    bool visible;
    lv_coord_t px, py;       /* where the icon is drawn */
    lv_coord_t ldx, ldy;     /* label offset from the icon, chosen by layout() */
    lv_color_t col;          /* colour applied to icon and trail (restyling redraws: only on change) */
    bool col_set;
    int label_pos;
    lv_obj_t *ol, *ic, *lbl, *trail;
    lv_point_t tp[TRAIL_N + 1];
    int ntp;
    int16_t prof[PROF_N];
    int nprof;
    uint32_t last_prof;
    uint32_t route_asked;
    uint8_t rare;
} slot_t;

static slot_t S[MAX_AC];
static int selected = -1;

static lv_obj_t *map_obj, *chip, *age_lbl, *count_lbl, *clock_lbl, *wifi_btn_lbl, *hint, *toast_lbl, *offline;
static lv_timer_t *toast_timer;
static int toast_slot = -1;          /* aircraft opened by tapping the toast */
static int toast_prio;               /* rare_t of the alert on screen: a lesser one does not replace it */
static int world_n;
static const char *last_source = "";
static uint32_t last_ok;
static bool api_down;

/* flight card */
static lv_obj_t *panel, *p_call, *p_line, *p_from, *p_to, *p_phase, *p_bar, *p_from_n, *p_to_n, *p_chart, *p_prof_lbl;
static lv_obj_t *p_val[6], *p_sub[6];
static lv_chart_series_t *p_ser;
/* list */
static lv_obj_t *list_panel, *list_rows;
/* Wi-Fi */
static lv_obj_t *wifi_scr, *wifi_list, *wifi_side, *pw_scr, *pw_title, *pw_ta, *pw_msg, *pw_go, *busy, *busy_lbl;
static char pw_ssid[33];

/* ---------- helpers ---------- */
static lv_color_t alt_color(int alt)
{
    static const int A[] = {0, 5000, 15000, 30000, 42000};
    static const uint8_t C[][3] = {{255, 157, 92}, {255, 209, 102}, {127, 227, 161}, {108, 201, 255}, {185, 167, 255}};
    if (alt < 0) alt = 0;
    if (alt > 42000) alt = 42000;
    for (int i = 1; i < 5; i++)
        if (alt <= A[i]) {
            float t = (float)(alt - A[i - 1]) / (A[i] - A[i - 1]);
            return lv_color_make(C[i - 1][0] + (C[i][0] - C[i - 1][0]) * t, C[i - 1][1] + (C[i][1] - C[i - 1][1]) * t,
                                 C[i - 1][2] + (C[i][2] - C[i - 1][2]) * t);
        }
    return lv_color_make(185, 167, 255);
}

static void proj(float lat, float lon, lv_coord_t *x, lv_coord_t *y)
{
    *x = (lv_coord_t)lroundf((lon - LON0) / (LON1 - LON0) * SCR_W);
    *y = (lv_coord_t)lroundf((LAT1 - lat) / (LAT1 - LAT0) * MAP_H);
}

static float dist_km(float lat1, float lon1, float lat2, float lon2)
{
    return hypotf((lat2 - lat1) * KM_LAT, (lon2 - lon1) * KM_LON);
}

static float gc_km(float lat1, float lon1, float lat2, float lon2)
{
    const float r = 0.0174533f;
    float a = sinf((lat2 - lat1) * r / 2), b = sinf((lon2 - lon1) * r / 2);
    float h = a * a + cosf(lat1 * r) * cosf(lat2 * r) * b * b;
    return 12742.0f * asinf(sqrtf(h));
}

static void thousands(char *out, size_t cap, long v)
{
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%ld", v < 0 ? -v : v);
    int len = (int)strlen(tmp), o = 0;
    if (v < 0 && o < (int)cap - 1) o += snprintf(out + o, cap - o, "\xE2\x88\x92");   /* minus sign */
    for (int i = 0; i < len && o < (int)cap - 3; i++) {
        if (i && (len - i) % 3 == 0) { out[o++] = '\xC2'; out[o++] = '\xA0'; }      /* no-break space */
        out[o++] = tmp[i];
    }
    out[o] = 0;
}

static lv_obj_t *box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt);
    return l;
}

/* Setting a label always redraws it: skip when the text is the same (most do not change each second) */
static void label_set(lv_obj_t *l, const char *t)
{
    const char *old = lv_label_get_text(l);
    if (!old || strcmp(old, t)) lv_label_set_text(l, t);
}

static void label_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    label_set(l, b);
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t h, bool primary,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, primary ? C_ACCENT : C_PANEL, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, C_LINE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, primary ? lv_color_hex(0xe0ab3a) : C_RAISE, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *l = label(b, &font_s16, primary ? C_INK : C_FG, txt);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void toast_hide(lv_timer_t *t) { (void)t; lv_obj_add_flag(lv_obj_get_parent(toast_lbl), LV_OBJ_FLAG_HIDDEN); toast_timer = NULL; toast_prio = 0; }

static void toast_ex(const char *txt, uint32_t ms, int slot, lv_color_t border)
{
    lv_obj_t *tb = lv_obj_get_parent(toast_lbl);
    label_set(toast_lbl, txt);
    lv_obj_set_style_border_color(tb, border, 0);
    lv_obj_set_style_border_width(tb, slot >= 0 ? 2 : 1, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tb);
    toast_slot = slot;
    toast_prio = 0;
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(toast_hide, ms, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void toast(const char *txt) { toast_ex(txt, 3500, -1, C_LINE); }

static const char *phase(const ac_t *a)
{
    if (abs(a->vs) < 150) return a->alt >= 20000 ? "Croisière" : "Palier";
    return a->vs > 0 ? "Montée" : "Descente";
}

/* ---------- rare aircraft: emergencies, military, giants ---------- */
typedef enum { RARE_NONE, RARE_BIG, RARE_MIL, RARE_EMERG } rare_t;

static const char *giant_name(const char *t)
{
    static const char *T[][2] = {
        {"A388", "A380"}, {"B741", "747"}, {"B742", "747"}, {"B743", "747"}, {"B744", "747"}, {"B748", "747-8"},
        {"B74S", "747SP"}, {"BLCF", "Dreamlifter"}, {"A124", "An-124"}, {"A225", "An-225"}, {"AN22", "An-22"},
        {"A3ST", "Beluga"}, {"A337", "BelugaXL"}, {"C5", "C-5 Galaxy"}, {"C5M", "C-5 Galaxy"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(t, T[i][0])) return T[i][1];
    return NULL;
}

/* Kind of rare aircraft, and its short on-screen tag */
static rare_t rare_of(const ac_t *a, const char **tag)
{
    static const char *E[][2] = {
        {"general", "URGENCE"}, {"lifeguard", "URGENCE MÉDICALE"}, {"minfuel", "CARBURANT BAS"},
        {"nordo", "PANNE RADIO"}, {"unlawful", "DÉTOURNEMENT"}, {"downed", "URGENCE"},
    };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        if (!strcmp(a->emergency, E[i][0])) { *tag = E[i][1]; return RARE_EMERG; }
    if (!strcmp(a->squawk, "7700")) { *tag = "URGENCE"; return RARE_EMERG; }
    if (!strcmp(a->squawk, "7600")) { *tag = "PANNE RADIO"; return RARE_EMERG; }
    if (!strcmp(a->squawk, "7500")) { *tag = "DÉTOURNEMENT"; return RARE_EMERG; }
    if (a->db_flags & 1) { *tag = "MILITAIRE"; return RARE_MIL; }
    if ((*tag = giant_name(a->type))) return RARE_BIG;
    *tag = "";
    return RARE_NONE;
}

static lv_color_t rare_color(rare_t r) { return r == RARE_EMERG ? C_EMERG : r == RARE_MIL ? C_MIL : C_BIG; }

/* Map label: callsign, plus a coloured tag for rare aircraft */
static void style_label(slot_t *s, bool sel)
{
    const char *tag;
    rare_t r = rare_of(&s->d, &tag);
    if (r) label_fmt(s->lbl, "%s · %s", s->d.call, tag);
    else label_set(s->lbl, s->d.call);
    bool dimmed = selected >= 0 && !sel;
    lv_color_t bg = sel ? C_FG : r ? rare_color(r) : C_BG;
    if (dimmed && r) bg = lv_color_mix(bg, C_BG, LV_OPA_40);
    lv_obj_set_style_bg_color(s->lbl, bg, 0);
    lv_obj_set_style_bg_opa(s->lbl, sel || r ? LV_OPA_COVER : LV_OPA_80, 0);
    lv_obj_set_style_text_color(s->lbl, sel || r ? C_INK : dimmed ? C_DIM : C_FG, 0);
    s->rare = r;
}

/* ---------- selection marker: ring on the selected aircraft, the others dimmed ---------- */
static lv_obj_t *sel_ring, *sel_edge;

static void layout(void);

static void mark_selection(void)
{
    if (!sel_ring) {
        sel_ring = lv_obj_create(map_obj);
        lv_obj_remove_style_all(sel_ring);
        lv_obj_set_size(sel_ring, 38, 38);
        lv_obj_set_style_radius(sel_ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(sel_ring, C_ACCENT, 0);
        lv_obj_set_style_border_width(sel_ring, 3, 0);
        lv_obj_clear_flag(sel_ring, LV_OBJ_FLAG_CLICKABLE);
        sel_edge = lv_label_create(map_obj);
        lv_obj_set_style_text_font(sel_edge, &font_s20, 0);
        lv_obj_set_style_text_color(sel_edge, C_ACCENT, 0);
        label_set(sel_edge, LV_SYMBOL_RIGHT);
    }
    for (int i = 0; i < MAX_AC; i++) {
        if (!S[i].used) continue;
        style_label(&S[i], i == selected);
        lv_img_set_zoom(S[i].ic, i == selected ? 320 : 256);
        lv_img_set_zoom(S[i].ol, i == selected ? 320 : 256);
    }
    lv_obj_add_flag(sel_edge, LV_OBJ_FLAG_HIDDEN);
    if (selected < 0) lv_obj_add_flag(sel_ring, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_clear_flag(sel_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sel_ring);    /* under the selected icon, moved up next */
    }
}

/* ---------- flight card ---------- */
static void close_panel(void)
{
    if (selected >= 0 && S[selected].used) {
        lv_obj_set_style_line_width(S[selected].trail, 2, 0);
        style_label(&S[selected], false);
    }
    selected = -1;
    mark_selection();
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void update_panel(void)
{
    if (selected < 0) return;
    slot_t *s = &S[selected];
    ac_t *a = &s->d;
    char b1[32], b2[48];
    thousands(b1, sizeof b1, (a->alt + 5) / 10 * 10);
    label_fmt(p_val[0], "%s ft", b1);
    thousands(b2, sizeof b2, (long)lroundf(a->alt * 0.3048f / 10) * 10);
    if (a->alt >= 8000) label_fmt(p_sub[0], "FL%03d · %s m", (a->alt + 50) / 100, b2);
    else label_fmt(p_sub[0], "%s m", b2);
    label_fmt(p_val[1], "%d kt", (int)lroundf(a->gs));
    thousands(b1, sizeof b1, lroundf(a->gs * 1.852f));
    label_fmt(p_sub[1], "%s km/h", b1);
    static const char *card[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
    label_fmt(p_val[2], "%03d°", (int)lroundf(a->trk) % 360);
    label_fmt(p_sub[2], "vers %s", card[(int)lroundf(a->trk / 45.0f) % 8]);
    int vs = (a->vs >= 0 ? a->vs + 25 : a->vs - 25) / 50 * 50;
    thousands(b1, sizeof b1, vs);
    label_fmt(p_val[3], "%s%s ft/min", vs > 0 ? "+" : "", b1);
    label_fmt(p_sub[3], "%d km de %s", (int)lroundf(dist_km(s->rlat, s->rlon, zone->ref_lat, zone->ref_lon)), zone->ref);
    const char *tag;
    rare_t rare = rare_of(a, &tag);
    if (rare) label_fmt(p_phase, "%s · %s", tag, phase(a));
    else label_set(p_phase, phase(a));
    lv_obj_set_style_text_color(p_phase, rare ? rare_color(rare) : alt_color(a->alt), 0);

    const route_t *r = adsb_route(a);
    if (r) {
        label_set(p_from, r->from);
        label_set(p_to, r->to);
        label_set(p_from_n, r->from_city);
        label_set(p_to_n, r->to_city);
        float da = gc_km(r->from_lat, r->from_lon, s->rlat, s->rlon), db = gc_km(s->rlat, s->rlon, r->to_lat, r->to_lon);
        bool half = r->from[0] == '?' || r->to[0] == '?';      /* one end unknown: no progress */
        lv_bar_set_value(p_bar, !half && da + db > 1 ? (int32_t)(da / (da + db) * 1000) : 0, LV_ANIM_OFF);
        if (r->flight[0] || r->airline[0])
            label_fmt(p_line, "%s%s%s", r->flight, r->flight[0] && r->airline[0] ? " · " : "", r->airline);
    } else {
        label_set(p_from, "—");
        label_set(p_to, "—");
        label_set(p_from_n, adsb_route_pending(a->call) && plat_ms() - s->route_asked < 15000 ? "Recherche de la route…" : "Route inconnue");
        label_set(p_to_n, "");
        lv_bar_set_value(p_bar, 0, LV_ANIM_OFF);
    }

    int n = s->nprof, mn = a->alt, mx = a->alt;
    for (int i = 0; i < n; i++) { if (s->prof[i] * 10 < mn) mn = s->prof[i] * 10; if (s->prof[i] * 10 > mx) mx = s->prof[i] * 10; }
    if (mx - mn < 1000) { int c = (mx + mn) / 2; mn = c - 500; mx = c + 500; }
    lv_chart_set_range(p_chart, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    lv_chart_set_point_count(p_chart, n + 1 < 2 ? 2 : n + 1);
    for (int i = 0; i < n; i++) lv_chart_set_value_by_id(p_chart, p_ser, i, s->prof[i] * 10);
    lv_chart_set_value_by_id(p_chart, p_ser, n, a->alt);
    if (n == 0) lv_chart_set_value_by_id(p_chart, p_ser, 1, a->alt);
    lv_chart_set_series_color(p_chart, p_ser, alt_color(a->alt));
    lv_chart_refresh(p_chart);
    thousands(b1, sizeof b1, (mn + 50) / 100 * 100);
    thousands(b2, sizeof b2, (mx + 50) / 100 * 100);
    label_fmt(p_prof_lbl, "Profil d'altitude · %s à %s ft", b1, b2);
}

static void open_panel(int i)
{
    if (selected >= 0) close_panel();
    selected = i;
    slot_t *s = &S[i];
    lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    label_set(p_call, s->d.call);
    label_set(p_line, s->d.desc[0] ? s->d.desc : s->d.type);
    label_set(p_val[4], s->d.reg[0] ? s->d.reg : "—");
    label_set(p_sub[4], s->d.type[0] ? s->d.type : "");
    label_set(p_val[5], s->d.squawk[0] ? s->d.squawk : "—");
    label_set(p_sub[5], s->d.hex);
    lv_obj_set_style_line_width(s->trail, 3, 0);
    style_label(s, true);
    mark_selection();
    lv_obj_move_foreground(s->ol);
    lv_obj_move_foreground(s->ic);
    lv_obj_move_foreground(s->lbl);
    layout();
    adsb_request_route(s->d.call, true);
    s->route_asked = plat_ms();
    update_panel();
}

static void close_cb(lv_event_t *e) { (void)e; close_panel(); }

static lv_obj_t *build_panel(lv_obj_t *scr)
{
    lv_obj_t *p = box(scr, SCR_W - PANEL_W, TOP_H, PANEL_W, MAP_H, C_PANEL);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_LINE, 0);
    p_call = label(p, &font_b24, C_FG, "");
    lv_obj_set_pos(p_call, 14, 10);
    p_line = label(p, &font_m12, C_DIM, "");
    lv_obj_set_pos(p_line, 14, 40);
    lv_obj_set_width(p_line, PANEL_W - 64);
    lv_label_set_long_mode(p_line, LV_LABEL_LONG_DOT);
    lv_obj_t *x = button(p, LV_SYMBOL_CLOSE, 44, 44, false, close_cb, NULL);
    lv_obj_set_style_border_width(x, 0, 0);
    lv_obj_set_pos(x, PANEL_W - 48, 4);

    p_from = label(p, &font_s20, C_FG, "");
    lv_obj_set_pos(p_from, 14, 64);
    p_to = label(p, &font_s20, C_FG, "");
    lv_obj_align(p_to, LV_ALIGN_TOP_RIGHT, -14, 64);
    p_phase = label(p, &font_m12, C_FG, "");
    lv_obj_set_style_bg_color(p_phase, C_RAISE, 0);
    lv_obj_set_style_bg_opa(p_phase, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(p_phase, 6, 0);
    lv_obj_set_style_pad_ver(p_phase, 2, 0);
    lv_obj_set_style_radius(p_phase, 3, 0);
    lv_obj_align(p_phase, LV_ALIGN_TOP_MID, 0, 68);
    p_bar = lv_bar_create(p);
    lv_obj_remove_style_all(p_bar);
    lv_obj_set_size(p_bar, PANEL_W - 28, 4);
    lv_obj_set_pos(p_bar, 14, 94);
    lv_obj_set_style_bg_color(p_bar, C_RAISE, 0);
    lv_obj_set_style_bg_opa(p_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p_bar, 2, 0);
    lv_obj_set_style_bg_color(p_bar, C_FG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(p_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(p_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_range(p_bar, 0, 1000);
    p_from_n = label(p, &font_m12, C_DIM, "");
    lv_obj_set_pos(p_from_n, 14, 102);
    p_to_n = label(p, &font_m12, C_DIM, "");
    lv_obj_align(p_to_n, LV_ALIGN_TOP_RIGHT, -14, 102);
    lv_obj_t *sep = box(p, 0, 124, PANEL_W, 1, C_LINE);
    (void)sep;

    static const char *names[] = {"Altitude", "Vitesse sol", "Cap", "Taux vertical", "Immatriculation", "Squawk · Hex"};
    for (int i = 0; i < 6; i++) {
        lv_coord_t cx = 14 + (i % 2) * 130, cy = 134 + (i / 2) * 62;
        lv_obj_t *t = label(p, &font_m12, C_DIM, names[i]);
        lv_obj_set_pos(t, cx, cy);
        p_val[i] = label(p, &font_s16, C_FG, "");
        lv_obj_set_pos(p_val[i], cx, cy + 15);
        p_sub[i] = label(p, &font_m12, C_DIM, "");
        lv_obj_set_pos(p_sub[i], cx, cy + 36);
    }
    box(p, 0, 322, PANEL_W, 1, C_LINE);
    p_prof_lbl = label(p, &font_m12, C_DIM, "");
    lv_obj_set_pos(p_prof_lbl, 14, 330);
    p_chart = lv_chart_create(p);
    lv_obj_remove_style_all(p_chart);
    lv_obj_set_size(p_chart, PANEL_W - 28, 80);
    lv_obj_set_pos(p_chart, 14, 350);
    lv_chart_set_type(p_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(p_chart, 0, 0);
    lv_chart_set_update_mode(p_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(p_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(p_chart, 2, LV_PART_ITEMS);
    p_ser = lv_chart_add_series(p_chart, C_FG, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ---------- list ---------- */
static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (S[i].used) open_panel(i);
}

static void render_list(void)
{
    lv_obj_clean(list_rows);
    int idx[MAX_AC], n = 0;
    for (int i = 0; i < MAX_AC; i++) if (S[i].used && S[i].visible) idx[n++] = i;
    for (int a = 1; a < n; a++)        /* sort by distance to the reference city */
        for (int b = a; b > 0; b--) {
            float d1 = dist_km(S[idx[b - 1]].rlat, S[idx[b - 1]].rlon, zone->ref_lat, zone->ref_lon);
            float d2 = dist_km(S[idx[b]].rlat, S[idx[b]].rlon, zone->ref_lat, zone->ref_lon);
            if (d2 < d1) { int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t; } else break;
        }
    for (int k = 0; k < n; k++) {
        slot_t *s = &S[idx[k]];
        lv_obj_t *r = lv_btn_create(list_rows);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, PANEL_W - 2, 50);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, C_RAISE, 0);
        lv_obj_set_style_bg_color(r, C_RAISE, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        const char *tag;
        rare_t rare = rare_of(&s->d, &tag);
        lv_obj_t *dot = box(r, 12, 20, 10, 10, rare ? rare_color(rare) : alt_color(s->d.alt));
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *c = label(r, &font_s16, C_FG, s->d.call);
        lv_obj_set_pos(c, 30, 7);
        char alt[24], sub[64];
        if (s->d.alt >= 8000) snprintf(alt, sizeof alt, "FL%03d", (s->d.alt + 50) / 100);
        else { thousands(sub, sizeof sub, (s->d.alt + 50) / 100 * 100); snprintf(alt, sizeof alt, "%s ft", sub); }
        lv_obj_t *al = label(r, &font_s16, C_FG, alt);
        lv_obj_align(al, LV_ALIGN_TOP_RIGHT, -12, 7);
        const route_t *rt = adsb_route(&s->d);
        int km = (int)lroundf(dist_km(s->rlat, s->rlon, zone->ref_lat, zone->ref_lon));
        char what[24];
        if (rt) snprintf(what, sizeof what, "%s → %s", rt->from, rt->to);
        else snprintf(what, sizeof what, "%s", s->d.type[0] ? s->d.type : "?");
        if (rare == RARE_BIG && !rt) snprintf(sub, sizeof sub, "%s · %d km", tag, km);
        else if (rare) snprintf(sub, sizeof sub, "%s · %s · %d km", tag, what, km);
        else snprintf(sub, sizeof sub, "%s · %d km", what, km);
        lv_obj_t *sl = label(r, &font_m12, rare ? rare_color(rare) : C_DIM, sub);
        lv_obj_set_pos(sl, 30, 29);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx[k]);
    }
}

static void list_close_cb(lv_event_t *e) { (void)e; lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN); }

static void list_btn_cb(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(list_panel, LV_OBJ_FLAG_HIDDEN)) {
        close_panel();
        render_list();
        lv_obj_clear_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *build_list(lv_obj_t *scr)
{
    lv_obj_t *p = box(scr, SCR_W - PANEL_W, TOP_H, PANEL_W, MAP_H, C_PANEL);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_LINE, 0);
    lv_obj_t *t = label(p, &font_s16, C_FG, "Avions dans la zone");
    lv_obj_set_pos(t, 14, 12);
    lv_obj_t *x = button(p, LV_SYMBOL_CLOSE, 44, 44, false, list_close_cb, NULL);
    lv_obj_set_style_border_width(x, 0, 0);
    lv_obj_set_pos(x, PANEL_W - 48, 0);
    box(p, 0, 44, PANEL_W, 1, C_LINE);
    list_rows = lv_obj_create(p);
    lv_obj_remove_style_all(list_rows);
    lv_obj_set_pos(list_rows, 1, 45);
    lv_obj_set_size(list_rows, PANEL_W - 1, MAP_H - 45);
    lv_obj_set_flex_flow(list_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_rows, LV_DIR_VER);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ---------- place: list under the title, map loaded into PSRAM ---------- */
const zone_t *zone = &zones[0];
int zone_gen;
static lv_obj_t *map_bg, *title_btn, *title_lbl, *zone_panel;
static lv_img_dsc_t map_dsc = {
    .header.cf = LV_IMG_CF_TRUE_COLOR, .header.w = SCR_W, .header.h = MAP_H, .data_size = SCR_W * MAP_H * 2,
};
static uint8_t *map_px;
static void slot_free(int i, const char *why);

static void zone_load(int i)
{
    if (i < 0 || i >= zones_n) i = 0;
    zone = &zones[i];
    if (!map_px) map_px = plat_big_alloc(map_dsc.data_size);
    if (!map_px || plat_inflate(zone->map, zone->map_len, map_px, map_dsc.data_size) != map_dsc.data_size)
        printf("[radar] map of %s: cannot inflate\n", zone->title);
    map_dsc.data = map_px;
    lv_img_cache_invalidate_src(&map_dsc);
    lv_img_set_src(map_bg, &map_dsc);
    lv_obj_invalidate(map_bg);
    label_fmt(title_lbl, "%s  " LV_SYMBOL_DOWN, zone->title);
    lv_obj_update_layout(title_btn);
    lv_coord_t x = lv_obj_get_width(title_btn) + 4;
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_align(age_lbl, LV_ALIGN_LEFT_MID, x + 140, 0);
}

static lv_obj_t *build_zones(lv_obj_t *scr);

static void zone_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_add_flag(zone_panel, LV_OBJ_FLAG_HIDDEN);
    if (&zones[i] == zone) return;
    close_panel();
    lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    for (int j = 0; j < MAX_AC; j++) if (S[j].used) slot_free(j, "");
    zone_gen++;
    zone_load(i);
    plat_setting_set("zone", i);
    adsb_zone_changed();
    last_ok = 0;
    printf("[radar] place: %s\n", zone->title);
}

static void title_cb(lv_event_t *e)
{
    (void)e;
    if (!lv_obj_has_flag(zone_panel, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(zone_panel, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_t *old = zone_panel;
    zone_panel = build_zones(lv_obj_get_parent(old));   /* rebuilt: the tick follows the current place */
    lv_obj_del(old);
    lv_obj_clear_flag(zone_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *build_zones(lv_obj_t *scr)
{
    const int row_h = 36, w = 250;
    lv_obj_t *p = box(scr, 0, TOP_H, w, row_h * zones_n + 8, C_PANEL);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_LINE, 0);
    lv_obj_set_style_radius(p, 0, 0);
    for (int i = 0; i < zones_n; i++) {
        lv_obj_t *r = lv_btn_create(p);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 0, 4 + i * row_h);
        lv_obj_set_size(r, w - 1, row_h);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(r, C_RAISE, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, zone_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        bool cur = &zones[i] == zone;
        lv_obj_t *l = label(r, &font_s16, cur ? C_ACCENT : C_FG, zones[i].title);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
        if (cur) {
            lv_obj_t *ok = label(r, &font_s16, C_ACCENT, LV_SYMBOL_OK);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -14, 0);
        }
    }
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ---------- aircraft on the map ---------- */
static void plane_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (S[i].used) open_panel(i);
}

static void slot_create(int i)
{
    slot_t *s = &S[i];
    const lv_img_dsc_t *fill = s->d.kind == KIND_HELI ? &img_heli : s->d.kind == KIND_LIGHT ? &img_light : &img_jet;
    const lv_img_dsc_t *ol = s->d.kind == KIND_HELI ? &img_heli_ol : s->d.kind == KIND_LIGHT ? &img_light_ol : &img_jet_ol;
    s->trail = lv_line_create(map_obj);
    lv_obj_set_style_line_width(s->trail, 2, 0);
    lv_obj_set_style_line_rounded(s->trail, true, 0);
    lv_obj_set_style_line_opa(s->trail, LV_OPA_60, 0);
    lv_obj_move_to_index(s->trail, 1);                       /* just above the background */
    s->ol = lv_img_create(map_obj);
    lv_img_set_src(s->ol, ol);
    lv_img_set_pivot(s->ol, 16, 16);
    lv_obj_set_style_img_recolor(s->ol, lv_color_hex(0x0b1519), 0);
    lv_obj_set_style_img_recolor_opa(s->ol, LV_OPA_COVER, 0);
    s->ic = lv_img_create(map_obj);
    lv_img_set_src(s->ic, fill);
    lv_img_set_pivot(s->ic, 16, 16);
    lv_obj_set_style_img_recolor_opa(s->ic, LV_OPA_COVER, 0);
    lv_obj_add_flag(s->ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s->ic, 10);
    lv_obj_add_event_cb(s->ic, plane_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s->lbl = label(map_obj, &font_m12, C_FG, s->d.call);
    lv_obj_set_style_pad_hor(s->lbl, 5, 0);
    lv_obj_set_style_pad_ver(s->lbl, 1, 0);
    lv_obj_add_flag(s->lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->lbl, plane_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s->ntp = 0;
    s->nprof = 0;
    s->label_pos = 0;
    style_label(s, false);
}

static void slot_free(int i, const char *why)
{
    slot_t *s = &S[i];
    if (i == toast_slot) toast_slot = -1;
    if (i == selected) {
        close_panel();
        char msg[48];
        snprintf(msg, sizeof msg, "%s %s", s->d.call, why);
        toast(msg);
    }
    lv_obj_del(s->trail);
    lv_obj_del(s->ol);
    lv_obj_del(s->ic);
    lv_obj_del(s->lbl);
    s->used = false;
}

/* labels: 4 slots around the icon, the first free one wins */
typedef struct { lv_coord_t x, y, w, h; } rect_t;
static bool overlap(rect_t a, rect_t b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

/* Dead reckoning from the last report, easing out the jump when a new one arrives */
static void position(slot_t *s, uint32_t now, lv_coord_t *x, lv_coord_t *y)
{
    float dt = (now - s->t) / 1000.0f, fwd = dt + s->d.seen_pos < 20 ? dt : 0;
    float km = s->d.gs * 0.514444f / 1000.0f * fwd, r = s->d.trk * 0.0174533f;
    float fade = dt < 2 ? 1 - dt / 2 : 0;
    s->rlat = s->d.lat + km * cosf(r) / KM_LAT + s->off_lat * fade;
    s->rlon = s->d.lon + km * sinf(r) / KM_LON + s->off_lon * fade;
    proj(s->rlat, s->rlon, x, y);
}

static void place_icon(int i, lv_coord_t x, lv_coord_t y)
{
    slot_t *s = &S[i];
    s->px = x; s->py = y;
    lv_obj_set_pos(s->ol, x - 16, y - 16);
    lv_obj_set_pos(s->ic, x - 16, y - 16);
    lv_obj_set_pos(s->lbl, x + s->ldx, y + s->ldy);
    if (i == selected && sel_ring) {
        lv_obj_set_pos(sel_ring, x - 19, y - 19);
        if (!lv_obj_has_flag(sel_edge, LV_OBJ_FLAG_HIDDEN)) lv_obj_set_y(sel_edge, y - 12);
    }
}

/* 10 times a second: only the aircraft whose pixel changed move (labels, colours, trails: layout) */
static void motion(lv_timer_t *t)
{
    (void)t;
    uint32_t now = plat_ms();
    for (int i = 0; i < MAX_AC; i++) {
        slot_t *s = &S[i];
        if (!s->used || !s->visible) continue;
        lv_coord_t x, y;
        position(s, now, &x, &y);
        if (x != s->px || y != s->py) place_icon(i, x, y);
    }
}

static void layout(void)
{
    static const int8_t LX[4] = {14, 14, -14, -14}, LY[4] = {-20, 4, -20, 4};
    rect_t placed[MAX_AC];
    int np = 0;
    uint32_t now = plat_ms();
    int order[MAX_AC], n = 0;
    if (selected >= 0) order[n++] = selected;
    for (int i = 0; i < MAX_AC; i++) if (S[i].used && i != selected) order[n++] = i;
    for (int k = 0; k < n; k++) {
        slot_t *s = &S[order[k]];
        lv_coord_t x, y;
        position(s, now, &x, &y);
        bool vis = x > -20 && x < SCR_W + 20 && y > -20 && y < MAP_H + 20;
        if (s->visible && !vis && order[k] == selected) { close_panel(); }
        s->visible = vis;
        lv_obj_t *objs[] = {s->ol, s->ic, s->lbl, s->trail};
        for (int j = 0; j < 4; j++) {
            if (vis) lv_obj_clear_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(objs[j], LV_OBJ_FLAG_HIDDEN);
        }
        if (!vis) continue;
        lv_color_t col = alt_color(s->d.alt);
        if (selected >= 0 && order[k] != selected) col = lv_color_mix(col, C_BG, LV_OPA_40);
        int16_t ang = (int16_t)lroundf(s->d.trk * 10) % 3600;
        lv_img_set_angle(s->ol, ang);
        lv_img_set_angle(s->ic, ang);
        if (!s->col_set || s->col.full != col.full) {
            lv_obj_set_style_img_recolor(s->ic, col, 0);
            lv_obj_set_style_line_color(s->trail, col, 0);
            s->col = col;
            s->col_set = true;
        }
        if (order[k] == selected && sel_ring) {
            bool under = x > SCR_W - PANEL_W - 8;      /* hidden by the card: point at it from the edge */
            if (under) {
                lv_obj_set_pos(sel_edge, SCR_W - PANEL_W - 22, y - 12);
                lv_obj_clear_flag(sel_edge, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(sel_edge, LV_OBJ_FLAG_HIDDEN);
            }
        }
        /* trail: one point every 6 px, the last one follows the aircraft (redrawn only when it moved) */
        bool moved = s->ntp == 0 || s->tp[s->ntp].x != x || s->tp[s->ntp].y != y;
        if (s->ntp == 0 || abs(s->tp[s->ntp - 1].x - x) + abs(s->tp[s->ntp - 1].y - y) > 6) {
            if (s->ntp == TRAIL_N) { memmove(s->tp, s->tp + 1, sizeof(lv_point_t) * (TRAIL_N - 1)); s->ntp--; }
            s->tp[s->ntp].x = x; s->tp[s->ntp].y = y; s->ntp++;
        }
        s->tp[s->ntp].x = x; s->tp[s->ntp].y = y;
        if (moved) lv_line_set_points(s->trail, s->tp, s->ntp + 1);
        lv_point_t sz;
        lv_txt_get_size(&sz, lv_label_get_text(s->lbl), &font_m12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        rect_t b = {0};
        int best = s->label_pos;
        for (int t = 0; t < 5; t++) {
            int pos = t == 0 ? s->label_pos : t - 1;
            b.w = sz.x + 10; b.h = sz.y + 2;
            b.x = LX[pos] > 0 ? x + LX[pos] : x + LX[pos] - b.w;
            b.y = y + LY[pos];
            bool hit = false;
            for (int q = 0; q < np && !hit; q++) hit = overlap(b, placed[q]);
            if (!hit) { best = pos; break; }
        }
        s->label_pos = best;
        b.x = LX[best] > 0 ? x + LX[best] : x + LX[best] - b.w;
        b.y = y + LY[best];
        placed[np++] = b;
        s->ldx = b.x - x; s->ldy = b.y - y;
        place_icon(order[k], x, y);
    }
}

/* Banner when a rare aircraft shows up; tapping it opens the flight card */
static void rare_alert(int i)
{
    slot_t *s = &S[i];
    const char *tag;
    rare_t r = rare_of(&s->d, &tag);
    char msg[96];
    const char *what = s->d.desc[0] ? s->d.desc : s->d.type;
    if (r == RARE_EMERG) snprintf(msg, sizeof msg, LV_SYMBOL_WARNING "  %s : %s", tag, s->d.call);
    else if (r == RARE_MIL) snprintf(msg, sizeof msg, "Avion militaire dans la zone : %s%s%s", s->d.call, what[0] ? " · " : "", what);
    else snprintf(msg, sizeof msg, "%s dans la zone : %s", tag, s->d.call);
    if (toast_timer && (int)r < toast_prio) return;
    toast_ex(msg, r == RARE_EMERG ? 20000 : 10000, i, rare_color(r));
    toast_prio = r;
}

static void toast_cb(lv_event_t *e)
{
    (void)e;
    int i = toast_slot;
    if (toast_timer) lv_timer_del(toast_timer);
    toast_hide(NULL);
    if (i >= 0 && S[i].used) open_panel(i);
}

void ui_world_count(int n) { world_n = n; }

void ui_apply_aircraft(const ac_t *list, int n, const char *source)
{
    uint32_t now = plat_ms();
    bool seen[MAX_AC] = {0};
    for (int k = 0; k < n; k++) {
        const ac_t *a = &list[k];
        int i = -1, free_i = -1;
        for (int j = 0; j < MAX_AC; j++) {
            if (S[j].used && !strcmp(S[j].d.hex, a->hex)) { i = j; break; }
            if (!S[j].used && free_i < 0) free_i = j;
        }
        if (i < 0) {
            if (free_i < 0) continue;
            i = free_i;
            memset(&S[i], 0, sizeof S[i]);
            S[i].used = true;
            S[i].d = *a;
            S[i].rlat = a->lat; S[i].rlon = a->lon;
            slot_create(i);
            if (S[i].rare) rare_alert(i);
        } else {
            float jump = dist_km(S[i].rlat, S[i].rlon, a->lat, a->lon);
            S[i].off_lat = jump < 5 ? S[i].rlat - a->lat : 0;
            S[i].off_lon = jump < 5 ? S[i].rlon - a->lon : 0;
            const char *tag;
            rare_t was = S[i].rare, now_r = rare_of(a, &tag);
            bool relabel = strcmp(S[i].d.call, a->call) || was != now_r;
            S[i].d = *a;
            if (relabel) style_label(&S[i], i == selected);
            if (now_r > was) rare_alert(i);
        }
        S[i].t = now;
        if (now - S[i].last_prof > 15000 || S[i].nprof == 0) {
            if (S[i].nprof == PROF_N) { memmove(S[i].prof, S[i].prof + 1, sizeof(int16_t) * (PROF_N - 1)); S[i].nprof--; }
            S[i].prof[S[i].nprof++] = (int16_t)(a->alt / 10);
            S[i].last_prof = now;
        }
        seen[i] = true;
    }
    for (int j = 0; j < MAX_AC; j++)
        if (S[j].used && !seen[j] && now - S[j].t > 30000) slot_free(j, "a quitté la zone");
    last_source = source;
    last_ok = now;
    api_down = false;
    lv_obj_add_flag(offline, LV_OBJ_FLAG_HIDDEN);
    layout();
    if (!lv_obj_has_flag(list_panel, LV_OBJ_FLAG_HIDDEN)) render_list();
}

void ui_fetch_failed(void)
{
    api_down = true;
    uint32_t now = plat_ms();
    for (int j = 0; j < MAX_AC; j++) if (S[j].used && now - S[j].t > 30000) slot_free(j, "n'est plus suivi");
}

void ui_route_ready(const char *callsign)
{
    if (selected >= 0 && !strcmp(S[selected].d.call, callsign)) update_panel();
}

/* ---------- Wi-Fi ---------- */
static plat_ap_t aps[24];
static volatile int aps_n = -1;
static volatile bool aps_ready;
static volatile int wifi_ev = -1;

void app_wifi_scan_done(const plat_ap_t *list, int n)
{
    if (n > 24) n = 24;
    memcpy(aps, list, sizeof(plat_ap_t) * n);
    aps_n = n;
    aps_ready = true;
}

void app_wifi_event(plat_net_state_t st) { wifi_ev = st; }

static char upd_msg[64];
static volatile bool upd_ready;
void app_update_status(const char *msg) { snprintf(upd_msg, sizeof upd_msg, "%s", msg); upd_ready = true; }

static void render_wifi_side(void);
static void open_pw(const char *ssid);

static void ap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char cur[33] = "", ip[16];
    int rssi;
    if (plat_wifi_info(cur, ip, &rssi) == NET_UP && !strcmp(cur, aps[i].ssid)) return;
    if (aps[i].secure) open_pw(aps[i].ssid);
    else {
        snprintf(pw_ssid, sizeof pw_ssid, "%s", aps[i].ssid);
        label_fmt(busy_lbl, "Connexion à %s…", pw_ssid);
        lv_obj_clear_flag(busy, LV_OBJ_FLAG_HIDDEN);
        plat_wifi_connect(pw_ssid, "");
    }
}

static void render_wifi_list(void)
{
    lv_obj_clean(wifi_list);
    if (aps_n < 0) {
        lv_obj_t *l = label(wifi_list, &font_m14, C_DIM, "Recherche des réseaux…");
        lv_obj_set_style_pad_all(l, 16, 0);
        return;
    }
    if (aps_n == 0) {
        lv_obj_t *l = label(wifi_list, &font_m14, C_DIM, "Aucun réseau 2,4 GHz trouvé.");
        lv_obj_set_style_pad_all(l, 16, 0);
        return;
    }
    char cur[33] = "", ip[16];
    int rssi;
    bool up = plat_wifi_info(cur, ip, &rssi) == NET_UP;
    for (int i = 0; i < aps_n; i++) {
        lv_obj_t *r = lv_btn_create(wifi_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), 58);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, C_RAISE, 0);
        lv_obj_set_style_bg_color(r, C_RAISE, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        int lvl = aps[i].rssi > -58 ? 4 : aps[i].rssi > -68 ? 3 : aps[i].rssi > -78 ? 2 : 1;
        for (int b = 0; b < 4; b++) {
            lv_obj_t *bar = box(r, 14 + b * 6, 37 - (6 + b * 3), 4, 6 + b * 3, b < lvl ? C_FG : C_LINE);
            lv_obj_set_style_radius(bar, 1, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t *nm = label(r, &font_s16, C_FG, aps[i].ssid);
        lv_obj_set_pos(nm, 52, 9);
        lv_obj_set_width(nm, 360);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        bool on = up && !strcmp(cur, aps[i].ssid);
        lv_obj_t *sub = label(r, &font_m12, on ? C_OK : C_DIM, on ? "Connecté" : aps[i].secure ? "Sécurisé" : "Ouvert");
        lv_obj_set_pos(sub, 52, 32);
        if (aps[i].secure) {
            lv_obj_t *lk = label(r, &font_m14, C_DIM, "\xEF\x80\xA3");      /* FontAwesome padlock 0xF023 */
            lv_obj_align(lk, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        lv_obj_add_event_cb(r, ap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void scan(void)
{
    aps_n = -1;
    render_wifi_list();
    plat_wifi_scan();
}

static void scan_cb(lv_event_t *e) { (void)e; scan(); }
static void wifi_back_cb(lv_event_t *e) { (void)e; lv_obj_add_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN); }

/* ---------- sleep: the screen saver takes over, a tap goes back to the map ---------- */
static void saver_show(void);

static void sleep_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN);
    if (selected >= 0) close_panel();
    saver_show();
}

static void open_wifi(void)
{
    lv_obj_clear_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_scr);
    render_wifi_side();
    scan();
}

static void wifi_btn_cb(lv_event_t *e) { (void)e; open_wifi(); }

static void update_cb(lv_event_t *e)
{
    (void)e;
    toast_ex("Recherche d'une mise à jour…", 4000, -1, C_LINE);
    plat_update_check();
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    plat_wifi_forget();
    close_panel();
    lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    render_wifi_side();
    render_wifi_list();
}

static void render_wifi_side(void)
{
    lv_obj_clean(wifi_side);
    char ssid[33] = "", ip[16] = "";
    int rssi = 0;
    plat_net_state_t st = plat_wifi_info(ssid, ip, &rssi);
    lv_obj_t *h = label(wifi_side, &font_m12, C_DIM, "CONNEXION");
    lv_obj_set_style_text_letter_space(h, 1, 0);
    if (st == NET_UP) {
        lv_obj_t *n = label(wifi_side, &font_s20, C_FG, ssid);
        lv_obj_set_width(n, 226);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        char line[64];
        snprintf(line, sizeof line, "Adresse IP   %s", ip);
        label(wifi_side, &font_m14, C_FG, line);
        snprintf(line, sizeof line, "Signal   %d dBm", rssi);
        label(wifi_side, &font_m14, C_FG, line);
        lv_obj_t *p = label(wifi_side, &font_m12, C_DIM, "Seuls les réseaux 2,4 GHz sont visibles.");
        lv_obj_set_width(p, 226);
        lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
        lv_obj_t *sp = lv_obj_create(wifi_side);
        lv_obj_remove_style_all(sp);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_t *v = label(wifi_side, &font_m12, C_DIM, "");
        label_fmt(v, "Version %s", plat_version());
        button(wifi_side, LV_SYMBOL_REFRESH "  Mise à jour", 226, 44, false, update_cb, NULL);
        button(wifi_side, "Oublier ce réseau", 226, 44, false, forget_cb, NULL);
        button(wifi_side, LV_SYMBOL_EYE_CLOSE "  Mettre en veille", 226, 44, true, sleep_cb, NULL);
    } else {
        label(wifi_side, &font_s20, C_FG, st == NET_CONNECTING ? "Connexion…" : "Non connecté");
        lv_obj_t *p = label(wifi_side, &font_m14, C_DIM,
                            "Choisissez un réseau dans la liste. Le mot de passe restera enregistré sur l'écran.\n\n"
                            "Seuls les réseaux 2,4 GHz sont visibles.");
        lv_obj_set_width(p, 226);
        lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
        lv_obj_t *sp = lv_obj_create(wifi_side);
        lv_obj_remove_style_all(sp);
        lv_obj_set_flex_grow(sp, 1);
        button(wifi_side, LV_SYMBOL_EYE_CLOSE "  Mettre en veille", 226, 44, true, sleep_cb, NULL);
    }
}

static lv_obj_t *topbar(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb, lv_obj_t **title_out)
{
    lv_obj_t *t = box(parent, 0, 0, SCR_W, 52, C_PANEL);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, C_LINE, 0);
    lv_obj_t *b = button(t, LV_SYMBOL_LEFT, 52, 52, false, back_cb, NULL);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_t *l = label(t, &font_s20, C_FG, title);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 60, 0);
    if (title_out) *title_out = l;
    return t;
}

static lv_obj_t *build_wifi(lv_obj_t *scr)
{
    lv_obj_t *w = box(scr, 0, 0, SCR_W, 480, C_BG);
    lv_obj_t *t = topbar(w, "Réglages", wifi_back_cb, NULL);
    lv_obj_t *r = button(t, "Actualiser", 120, 40, false, scan_cb, NULL);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -8, 0);
    wifi_list = lv_obj_create(w);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_pos(wifi_list, 0, 52);
    lv_obj_set_size(wifi_list, SCR_W - 256, 480 - 52);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);
    lv_obj_set_style_border_side(wifi_list, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(wifi_list, 1, 0);
    lv_obj_set_style_border_color(wifi_list, C_LINE, 0);
    wifi_side = lv_obj_create(w);
    lv_obj_remove_style_all(wifi_side);
    lv_obj_set_pos(wifi_side, SCR_W - 256, 52);
    lv_obj_set_size(wifi_side, 256, 480 - 52);
    lv_obj_set_style_pad_all(wifi_side, 15, 0);
    lv_obj_set_style_pad_row(wifi_side, 10, 0);
    lv_obj_set_flex_flow(wifi_side, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wifi_side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
    return w;
}

/* ---------- password ---------- */
static const char *kb_lower[] = {"a", "z", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                                 "q", "s", "d", "f", "g", "h", "j", "k", "l", "m", "\n",
                                 "ABC", "w", "x", "c", "v", "b", "n", "'", LV_SYMBOL_BACKSPACE, "\n",
                                 "1#", " ", ".", ""};
static const char *kb_upper[] = {"A", "Z", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                                 "Q", "S", "D", "F", "G", "H", "J", "K", "L", "M", "\n",
                                 "abc", "W", "X", "C", "V", "B", "N", "'", LV_SYMBOL_BACKSPACE, "\n",
                                 "1#", " ", ".", ""};
static const char *kb_num[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                               "@", "#", "&", "_", "-", "+", "(", ")", "/", "*", "\n",
                               "abc", "!", "?", ",", ":", ";", "\"", "=", LV_SYMBOL_BACKSPACE, "\n",
                               "ABC", " ", "%", ""};
static const lv_btnmatrix_ctrl_t kb_ctrl[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                              1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                              3 | LV_BTNMATRIX_CTRL_NO_REPEAT, 2, 2, 2, 2, 2, 2, 2, 3,
                                              3 | LV_BTNMATRIX_CTRL_NO_REPEAT, 10, 3};

static void pw_changed(void)
{
    size_t n = strlen(lv_textarea_get_text(pw_ta));
    if (n >= 8) {
        lv_obj_clear_state(pw_go, LV_STATE_DISABLED);
        label_fmt(pw_msg, "%u caractères", (unsigned)n);
    } else {
        lv_obj_add_state(pw_go, LV_STATE_DISABLED);
        if (n) label_fmt(pw_msg, "Encore %u caractère%s (8 minimum)", (unsigned)(8 - n), 8 - n > 1 ? "s" : "");
        else label_set(pw_msg, "8 caractères minimum");
    }
    lv_obj_set_style_text_color(pw_msg, C_DIM, 0);
}

static void ta_cb(lv_event_t *e) { (void)e; pw_changed(); }
static void pw_back_cb(lv_event_t *e) { (void)e; lv_obj_add_flag(pw_scr, LV_OBJ_FLAG_HIDDEN); }

static void eye_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_obj_get_child(lv_event_get_target(e), 0);
    bool hide = !lv_textarea_get_password_mode(pw_ta);
    lv_textarea_set_password_mode(pw_ta, hide);
    label_set(l, hide ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    label_fmt(busy_lbl, "Connexion à %s…", pw_ssid);
    lv_obj_clear_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(busy);
    plat_wifi_connect(pw_ssid, lv_textarea_get_text(pw_ta));
}

static void open_pw(const char *ssid)
{
    snprintf(pw_ssid, sizeof pw_ssid, "%s", ssid);
    label_set(pw_title, ssid);
    lv_textarea_set_text(pw_ta, "");
    lv_textarea_set_password_mode(pw_ta, true);
    pw_changed();
    lv_obj_clear_flag(pw_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pw_scr);
}

static lv_obj_t *build_pw(lv_obj_t *scr)
{
    lv_obj_t *w = box(scr, 0, 0, SCR_W, 480, C_BG);
    lv_obj_t *t = topbar(w, "", pw_back_cb, &pw_title);
    pw_go = button(t, "Se connecter", 150, 40, true, go_cb, NULL);
    lv_obj_align(pw_go, LV_ALIGN_RIGHT_MID, -8, 0);
    pw_ta = lv_textarea_create(w);
    lv_obj_set_pos(pw_ta, 12, 62);
    lv_obj_set_size(pw_ta, SCR_W - 24, 50);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_max_length(pw_ta, 63);
    lv_textarea_set_placeholder_text(pw_ta, "Mot de passe");
    lv_obj_set_style_bg_color(pw_ta, C_PANEL, 0);
    lv_obj_set_style_border_color(pw_ta, C_LINE, 0);
    lv_obj_set_style_border_color(pw_ta, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(pw_ta, C_FG, 0);
    lv_obj_set_style_text_font(pw_ta, &font_s20, 0);
    lv_obj_set_style_pad_right(pw_ta, 56, 0);
    lv_obj_add_state(pw_ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(pw_ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *eye = button(w, LV_SYMBOL_EYE_OPEN, 50, 48, false, eye_cb, NULL);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_pos(eye, SCR_W - 64, 63);
    pw_msg = label(w, &font_m12, C_DIM, "");
    lv_obj_set_pos(pw_msg, 14, 116);
    lv_obj_t *kb = lv_keyboard_create(w);
    lv_obj_set_size(kb, SCR_W, 240);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_num, kb_ctrl);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, pw_ta);
    lv_obj_set_style_bg_color(kb, C_PANEL, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_gap(kb, 5, 0);
    lv_obj_set_style_bg_color(kb, C_RAISE, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, C_LINE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, C_RAISE, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, C_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &font_s20, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
    busy = box(w, 0, 52, SCR_W, 480 - 52, C_BG);
    lv_obj_t *sp = lv_spinner_create(busy, 1000, 60);
    lv_obj_set_size(sp, 40, 40);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(sp, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, C_LINE, 0);
    busy_lbl = label(busy, &font_s16, C_FG, "");
    lv_obj_align(busy_lbl, LV_ALIGN_CENTER, 0, 28);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
    return w;
}

/* ---------- top bar, map, timers ---------- */
static void map_click_cb(lv_event_t *e)
{
    (void)e;
    close_panel();
    lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(zone_panel, LV_OBJ_FLAG_HIDDEN);
}

static void offline_btn_cb(lv_event_t *e) { (void)e; open_wifi(); }
static void hint_hide(lv_timer_t *t) { (void)t; lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN); }

static void set_chip(const char *txt, lv_color_t bg)
{
    label_set(chip, txt);
    lv_obj_set_style_bg_color(chip, bg, 0);
}

/* ---------- screen saver (sleep mode): a big clock on black, the worldwide count below ---------- */
static lv_obj_t *sv, *sv_box, *sv_dig[4], *sv_dot[2], *sv_world;
static int sv_min = -1;

static void saver_hide(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(sv, LV_OBJ_FLAG_HIDDEN);
}

static void build_saver(void)
{
    /* overlapping rounded digits in two blues, translucent so the overlaps show; two light dots for the colon */
    static const uint32_t COL[4] = {0x1c4384, 0x4a6c95, 0x23498a, 0x547396};   /* dimmed: the backlight is on/off only */
    sv = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sv);
    lv_obj_set_size(sv, SCR_W, 480);
    lv_obj_set_style_bg_color(sv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sv, LV_OPA_COVER, 0);
    lv_obj_add_flag(sv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sv, saver_hide, LV_EVENT_CLICKED, NULL);
    sv_box = lv_obj_create(sv);
    lv_obj_remove_style_all(sv_box);
    lv_obj_set_size(sv_box, SCR_W, 480);
    lv_obj_clear_flag(sv_box, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; i++) {
        sv_dig[i] = label(sv_box, &font_clock, lv_color_hex(COL[i]), "");
        lv_obj_set_style_text_opa(sv_dig[i], 225, 0);
    }
    for (int i = 0; i < 2; i++) {
        sv_dot[i] = lv_obj_create(sv_box);
        lv_obj_remove_style_all(sv_dot[i]);
        lv_obj_set_size(sv_dot[i], 46, 46);
        lv_obj_set_style_radius(sv_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sv_dot[i], lv_color_hex(0x868a8f), 0);
        lv_obj_set_style_bg_opa(sv_dot[i], 235, 0);
    }
    sv_world = label(sv_box, &font_s20, lv_color_hex(0x535b64), "");
}

static void saver_update(void)
{
    int h, m;
    if (!plat_local_time(&h, &m)) h = m = 0;
    if (h * 60 + m != sv_min) {                       /* redraw once a minute; the whole clock drifts a little */
        sv_min = h * 60 + m;
        static const int8_t DX[] = {0, 8, 0, -8}, DY[] = {-6, 0, 6, 0};
        lv_obj_set_pos(sv_box, DX[m % 4], DY[m % 4]);
        char d[4] = {(char)('0' + h / 10), (char)('0' + h % 10), (char)('0' + m / 10), (char)('0' + m % 10)};
        const int STEP = 148, GAP = 178, Y = 78;     /* digits ~185 px wide: neighbours overlap */
        bool one = h < 10;                            /* 9:05, not 09:05 */
        int w = (one ? 0 : STEP) + GAP + STEP + 185;
        int x = (SCR_W - w) / 2;
        for (int i = 0; i < 4; i++) {
            if (i == 0 && one) { lv_obj_add_flag(sv_dig[0], LV_OBJ_FLAG_HIDDEN); continue; }
            lv_obj_clear_flag(sv_dig[i], LV_OBJ_FLAG_HIDDEN);
            char t[2] = {d[i], 0};
            label_set(sv_dig[i], t);
            lv_obj_set_pos(sv_dig[i], x, Y);
            if (i == 1) {                             /* colon centred in the wider gap */
                int cx = x + 185 + (GAP - 185) / 2;
                lv_obj_set_pos(sv_dot[0], cx - 23, Y + 62);
                lv_obj_set_pos(sv_dot[1], cx - 23, Y + 150);
            }
            x += i == 1 ? GAP : STEP;
        }
    }
    if (world_n > 0) {
        char w[16];
        thousands(w, sizeof w, world_n);
        label_fmt(sv_world, "%s avions dans le monde", w);
        lv_obj_align(sv_world, LV_ALIGN_TOP_MID, 0, 372);
    }
}

static void saver_show(void)
{
    if (!sv) build_saver();
    lv_obj_clear_flag(sv, LV_OBJ_FLAG_HIDDEN);
    sv_min = -1;
    saver_update();
}

static void saver_tick(void)
{
    if (sv && !lv_obj_has_flag(sv, LV_OBJ_FLAG_HIDDEN)) saver_update();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    n++;
    layout();
    saver_tick();
    if (selected >= 0) update_panel();
    if (!lv_obj_has_flag(list_panel, LV_OBJ_FLAG_HIDDEN) && n % 4 == 0 && !lv_obj_has_state(list_rows, LV_STATE_PRESSED)) render_list();

    char ssid[33], ip[16];
    int rssi;
    plat_net_state_t st = plat_wifi_info(ssid, ip, &rssi);
    int vis = 0;
    for (int i = 0; i < MAX_AC; i++) vis += S[i].used && S[i].visible;
    label_fmt(count_lbl, LV_SYMBOL_LIST "  %d avion%s", vis, vis > 1 ? "s" : "");
    if (st != NET_UP) {
        set_chip("HORS LIGNE", C_ERR);
        label_set(age_lbl, "");
        lv_obj_set_style_text_color(wifi_btn_lbl, C_ERR, 0);
        if (!plat_wifi_has_saved() || st == NET_FAILED) lv_obj_clear_flag(offline, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_color(wifi_btn_lbl, C_FG, 0);
        lv_obj_add_flag(offline, LV_OBJ_FLAG_HIDDEN);
        if (!last_ok) { set_chip("CHARGEMENT", C_ACCENT); label_set(age_lbl, ""); }
        else {
            set_chip(api_down ? "API INJOIGNABLE" : "EN DIRECT", api_down ? C_ERR : C_OK);
            if (world_n > 0 && !api_down) {
                char w[16];
                thousands(w, sizeof w, world_n);
                label_fmt(age_lbl, "%s avions suivis dans le monde", w);
            } else {
                label_fmt(age_lbl, "%s · il y a %u s", last_source, (unsigned)((plat_ms() - last_ok) / 1000));
            }
        }
    }
    int h, m;
    if (plat_local_time(&h, &m)) label_fmt(clock_lbl, "%02d:%02d", h, m);
}

static void events(lv_timer_t *t)
{
    (void)t;
    if (upd_ready) {
        upd_ready = false;
        toast_ex(upd_msg, 6000, -1, C_ACCENT);
    }
    if (aps_ready) {
        aps_ready = false;
        if (!lv_obj_has_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN)) render_wifi_list();
    }
    int ev = wifi_ev;
    if (ev < 0) return;
    wifi_ev = -1;
    bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    if (ev == NET_UP) {
        if (waiting) {
            lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pw_scr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN);
            char msg[64];
            snprintf(msg, sizeof msg, "Connecté à %s", pw_ssid);
            toast(msg);
        }
        render_wifi_side();
    } else if (ev == NET_FAILED && waiting) {
        lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
        if (!lv_obj_has_flag(pw_scr, LV_OBJ_FLAG_HIDDEN)) {
            label_set(pw_msg, "Connexion impossible : vérifiez le mot de passe.");
            lv_obj_set_style_text_color(pw_msg, C_ERR, 0);
        } else toast("Connexion impossible");
        render_wifi_side();
    }
}

void radar_app_start(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    map_obj = box(scr, 0, TOP_H, SCR_W, MAP_H, C_BG);
    lv_obj_add_flag(map_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_obj, map_click_cb, LV_EVENT_CLICKED, NULL);
    map_bg = lv_img_create(map_obj);

    lv_obj_t *bar = box(scr, 0, 0, SCR_W, TOP_H, C_PANEL);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, C_LINE, 0);
    title_btn = lv_btn_create(bar);
    lv_obj_remove_style_all(title_btn);
    lv_obj_set_height(title_btn, TOP_H);
    lv_obj_set_width(title_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(title_btn, 12, 0);
    lv_obj_set_style_bg_opa(title_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(title_btn, C_RAISE, LV_STATE_PRESSED);
    lv_obj_add_event_cb(title_btn, title_cb, LV_EVENT_CLICKED, NULL);
    title_lbl = label(title_btn, &font_s16, C_FG, "");
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    chip = label(bar, &font_m12, C_INK, "");
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, 5, 0);
    lv_obj_set_style_pad_ver(chip, 2, 0);
    lv_obj_set_style_radius(chip, 3, 0);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, 150, 0);
    age_lbl = label(bar, &font_m12, C_DIM, "");
    lv_obj_align(age_lbl, LV_ALIGN_LEFT_MID, 290, 0);
    lv_obj_t *wb = button(bar, LV_SYMBOL_WIFI, 50, TOP_H, false, wifi_btn_cb, NULL);
    lv_obj_set_style_border_width(wb, 0, 0);
    lv_obj_set_style_radius(wb, 0, 0);
    lv_obj_align(wb, LV_ALIGN_RIGHT_MID, 0, 0);
    wifi_btn_lbl = lv_obj_get_child(wb, 0);
    clock_lbl = label(bar, &font_s16, C_DIM, "--:--");
    lv_obj_align(clock_lbl, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_t *lb = button(bar, "", 130, TOP_H, false, list_btn_cb, NULL);
    lv_obj_set_style_border_width(lb, 0, 0);
    lv_obj_set_style_radius(lb, 0, 0);
    lv_obj_align(lb, LV_ALIGN_RIGHT_MID, -112, 0);
    count_lbl = lv_obj_get_child(lb, 0);

    hint = label(scr, &font_s16, C_FG, "Touchez un avion pour voir ses données");
    lv_obj_set_style_bg_color(hint, C_PANEL, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(hint, 12, 0);
    lv_obj_set_style_pad_ver(hint, 6, 0);
    lv_obj_set_style_radius(hint, 5, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, TOP_H + 10);
    lv_timer_t *ht = lv_timer_create(hint_hide, 9000, NULL);
    lv_timer_set_repeat_count(ht, 1);

    panel = build_panel(scr);
    list_panel = build_list(scr);
    zone_panel = build_zones(scr);
    zone_load(plat_setting_get("zone", 0));

    offline = box(scr, 0, TOP_H, SCR_W, MAP_H, C_BG);
    lv_obj_set_style_bg_opa(offline, LV_OPA_70, 0);
    lv_obj_t *card = box(offline, 0, 0, 340, 150, C_PANEL);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, C_LINE, 0);
    lv_obj_t *ot = label(card, &font_s16, C_FG, "Pas de connexion Wi-Fi");
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *op = label(card, &font_m12, C_DIM, "Les avions s'afficheront dès que l'écran sera connecté.");
    lv_obj_set_width(op, 300);
    lv_obj_set_style_text_align(op, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(op, LV_LABEL_LONG_WRAP);
    lv_obj_align(op, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_t *ob = button(card, "Choisir un réseau", 200, 44, true, offline_btn_cb, NULL);
    lv_obj_align(ob, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(offline, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *tb = box(scr, 12, TOP_H + 10, 10, 10, C_RAISE);
    lv_obj_set_size(tb, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(tb, 5, 0);
    lv_obj_set_style_pad_hor(tb, 12, 0);
    lv_obj_set_style_pad_ver(tb, 8, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_color(tb, C_LINE, 0);
    toast_lbl = label(tb, &font_s16, C_FG, "");
    lv_obj_add_flag(tb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tb, toast_cb, LV_EVENT_CLICKED, NULL);

    wifi_scr = build_wifi(scr);
    pw_scr = build_pw(scr);

    lv_timer_create(tick, 1000, NULL);
    lv_timer_create(motion, 100, NULL);
    lv_timer_create(events, 100, NULL);
    tick(NULL);
    adsb_start();
    if (!plat_wifi_has_saved()) open_wifi();
}

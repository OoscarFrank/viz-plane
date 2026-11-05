/* LVGL 8 radar UI: map, aircraft, flight card, list, Wi-Fi settings.
 * Same layout as the web simulator (radar.py), at 800x480. On-screen text stays in French. */
#include <math.h>
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

#define PANEL_W 276
#define TRAIL_N 24
#define PROF_N 60
#define KM_LAT 111.32f
#define KM_LON 75.04f   /* 111.32 x cos(47.67°) */

typedef struct {
    bool used;
    ac_t d;
    uint32_t t;              /* when the last position arrived */
    float off_lat, off_lon;  /* offset to absorb for smooth motion */
    float rlat, rlon;        /* displayed position */
    bool visible;
    int label_pos;
    lv_obj_t *ol, *ic, *lbl, *trail;
    lv_point_t tp[TRAIL_N + 1];
    int ntp;
    int16_t prof[PROF_N];
    int nprof;
    uint32_t last_prof;
    uint32_t route_asked;
} slot_t;

static slot_t S[MAX_AC];
static int selected = -1;

static lv_obj_t *map_obj, *chip, *age_lbl, *count_lbl, *clock_lbl, *wifi_btn_lbl, *hint, *toast_lbl, *offline;
static lv_timer_t *toast_timer;
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

static void toast_hide(lv_timer_t *t) { (void)t; lv_obj_add_flag(lv_obj_get_parent(toast_lbl), LV_OBJ_FLAG_HIDDEN); toast_timer = NULL; }

static void toast(const char *txt)
{
    lv_label_set_text(toast_lbl, txt);
    lv_obj_clear_flag(lv_obj_get_parent(toast_lbl), LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(toast_hide, 3500, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static const char *phase(const ac_t *a)
{
    if (abs(a->vs) < 150) return a->alt >= 20000 ? "Croisière" : "Palier";
    return a->vs > 0 ? "Montée" : "Descente";
}

/* ---------- flight card ---------- */
static void close_panel(void)
{
    if (selected >= 0 && S[selected].used) {
        lv_obj_set_style_line_width(S[selected].trail, 2, 0);
        lv_obj_set_style_bg_color(S[selected].lbl, C_BG, 0);
        lv_obj_set_style_text_color(S[selected].lbl, C_FG, 0);
    }
    selected = -1;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void update_panel(void)
{
    if (selected < 0) return;
    slot_t *s = &S[selected];
    ac_t *a = &s->d;
    char b1[32], b2[48];
    thousands(b1, sizeof b1, (a->alt + 5) / 10 * 10);
    lv_label_set_text_fmt(p_val[0], "%s ft", b1);
    thousands(b2, sizeof b2, (long)lroundf(a->alt * 0.3048f / 10) * 10);
    if (a->alt >= 8000) lv_label_set_text_fmt(p_sub[0], "FL%03d · %s m", (a->alt + 50) / 100, b2);
    else lv_label_set_text_fmt(p_sub[0], "%s m", b2);
    lv_label_set_text_fmt(p_val[1], "%d kt", (int)lroundf(a->gs));
    thousands(b1, sizeof b1, lroundf(a->gs * 1.852f));
    lv_label_set_text_fmt(p_sub[1], "%s km/h", b1);
    static const char *card[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
    lv_label_set_text_fmt(p_val[2], "%03d°", (int)lroundf(a->trk) % 360);
    lv_label_set_text_fmt(p_sub[2], "vers %s", card[(int)lroundf(a->trk / 45.0f) % 8]);
    int vs = (a->vs >= 0 ? a->vs + 25 : a->vs - 25) / 50 * 50;
    thousands(b1, sizeof b1, vs);
    lv_label_set_text_fmt(p_val[3], "%s%s ft/min", vs > 0 ? "+" : "", b1);
    lv_label_set_text_fmt(p_sub[3], "%d km de Mulhouse", (int)lroundf(dist_km(s->rlat, s->rlon, MUL_LAT, MUL_LON)));
    lv_label_set_text(p_phase, phase(a));
    lv_obj_set_style_text_color(p_phase, alt_color(a->alt), 0);

    const route_t *r = adsb_route(a);
    if (r) {
        lv_label_set_text(p_from, r->from);
        lv_label_set_text(p_to, r->to);
        lv_label_set_text(p_from_n, r->from_city);
        lv_label_set_text(p_to_n, r->to_city);
        float da = gc_km(r->from_lat, r->from_lon, s->rlat, s->rlon), db = gc_km(s->rlat, s->rlon, r->to_lat, r->to_lon);
        lv_bar_set_value(p_bar, da + db > 1 ? (int32_t)(da / (da + db) * 1000) : 0, LV_ANIM_OFF);
        if (r->flight[0] || r->airline[0])
            lv_label_set_text_fmt(p_line, "%s%s%s", r->flight, r->flight[0] && r->airline[0] ? " · " : "", r->airline);
    } else {
        lv_label_set_text(p_from, "—");
        lv_label_set_text(p_to, "—");
        lv_label_set_text(p_from_n, adsb_route_pending(a->call) && plat_ms() - s->route_asked < 15000 ? "Recherche de la route…" : "Route inconnue");
        lv_label_set_text(p_to_n, "");
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
    lv_label_set_text_fmt(p_prof_lbl, "Profil d'altitude · %s à %s ft", b1, b2);
}

static void open_panel(int i)
{
    if (selected >= 0) close_panel();
    selected = i;
    slot_t *s = &S[i];
    lv_obj_add_flag(list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(p_call, s->d.call);
    lv_label_set_text(p_line, s->d.desc[0] ? s->d.desc : s->d.type);
    lv_label_set_text(p_val[4], s->d.reg[0] ? s->d.reg : "—");
    lv_label_set_text(p_sub[4], s->d.type[0] ? s->d.type : "");
    lv_label_set_text(p_val[5], s->d.squawk[0] ? s->d.squawk : "—");
    lv_label_set_text(p_sub[5], s->d.hex);
    lv_obj_set_style_line_width(s->trail, 3, 0);
    lv_obj_set_style_bg_color(s->lbl, C_FG, 0);
    lv_obj_set_style_text_color(s->lbl, C_INK, 0);
    lv_obj_move_foreground(s->ol);
    lv_obj_move_foreground(s->ic);
    lv_obj_move_foreground(s->lbl);
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
    for (int a = 1; a < n; a++)        /* sort by distance to Mulhouse */
        for (int b = a; b > 0; b--) {
            float d1 = dist_km(S[idx[b - 1]].rlat, S[idx[b - 1]].rlon, MUL_LAT, MUL_LON);
            float d2 = dist_km(S[idx[b]].rlat, S[idx[b]].rlon, MUL_LAT, MUL_LON);
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
        lv_obj_t *dot = box(r, 12, 20, 10, 10, alt_color(s->d.alt));
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
        int km = (int)lroundf(dist_km(s->rlat, s->rlon, MUL_LAT, MUL_LON));
        if (rt) snprintf(sub, sizeof sub, "%s → %s · %d km", rt->from, rt->to, km);
        else snprintf(sub, sizeof sub, "%s · %d km", s->d.type[0] ? s->d.type : "?", km);
        lv_obj_t *sl = label(r, &font_m12, C_DIM, sub);
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
    lv_obj_set_style_bg_color(s->lbl, C_BG, 0);
    lv_obj_set_style_bg_opa(s->lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(s->lbl, 5, 0);
    lv_obj_set_style_pad_ver(s->lbl, 1, 0);
    lv_obj_add_flag(s->lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->lbl, plane_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s->ntp = 0;
    s->nprof = 0;
    s->label_pos = 0;
}

static void slot_free(int i, const char *why)
{
    slot_t *s = &S[i];
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
        float dt = (now - s->t) / 1000.0f, fwd = dt + s->d.seen_pos < 20 ? dt : 0;
        float km = s->d.gs * 0.514444f / 1000.0f * fwd, r = s->d.trk * 0.0174533f;
        float fade = dt < 2 ? 1 - dt / 2 : 0;
        s->rlat = s->d.lat + km * cosf(r) / KM_LAT + s->off_lat * fade;
        s->rlon = s->d.lon + km * sinf(r) / KM_LON + s->off_lon * fade;
        lv_coord_t x, y;
        proj(s->rlat, s->rlon, &x, &y);
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
        int16_t ang = (int16_t)lroundf(s->d.trk * 10) % 3600;
        lv_obj_set_pos(s->ol, x - 16, y - 16);
        lv_obj_set_pos(s->ic, x - 16, y - 16);
        lv_img_set_angle(s->ol, ang);
        lv_img_set_angle(s->ic, ang);
        lv_obj_set_style_img_recolor(s->ic, col, 0);
        /* trail: one point every 6 px, the last one follows the aircraft */
        if (s->ntp == 0 || abs(s->tp[s->ntp - 1].x - x) + abs(s->tp[s->ntp - 1].y - y) > 6) {
            if (s->ntp == TRAIL_N) { memmove(s->tp, s->tp + 1, sizeof(lv_point_t) * (TRAIL_N - 1)); s->ntp--; }
            s->tp[s->ntp].x = x; s->tp[s->ntp].y = y; s->ntp++;
        }
        s->tp[s->ntp].x = x; s->tp[s->ntp].y = y;
        lv_line_set_points(s->trail, s->tp, s->ntp + 1);
        lv_obj_set_style_line_color(s->trail, col, 0);
        lv_point_t sz;
        lv_txt_get_size(&sz, s->d.call, &font_m12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
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
        lv_obj_set_pos(s->lbl, b.x, b.y);
    }
}

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
        } else {
            float jump = dist_km(S[i].rlat, S[i].rlon, a->lat, a->lon);
            S[i].off_lat = jump < 5 ? S[i].rlat - a->lat : 0;
            S[i].off_lon = jump < 5 ? S[i].rlon - a->lon : 0;
            if (strcmp(S[i].d.call, a->call)) lv_label_set_text(S[i].lbl, a->call);
            S[i].d = *a;
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
        lv_label_set_text_fmt(busy_lbl, "Connexion à %s…", pw_ssid);
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

static void open_wifi(void)
{
    lv_obj_clear_flag(wifi_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_scr);
    render_wifi_side();
    scan();
}

static void wifi_btn_cb(lv_event_t *e) { (void)e; open_wifi(); }

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
        button(wifi_side, "Oublier ce réseau", 226, 44, false, forget_cb, NULL);
    } else {
        label(wifi_side, &font_s20, C_FG, st == NET_CONNECTING ? "Connexion…" : "Non connecté");
        lv_obj_t *p = label(wifi_side, &font_m14, C_DIM,
                            "Choisissez un réseau dans la liste. Le mot de passe restera enregistré sur l'écran.\n\n"
                            "Seuls les réseaux 2,4 GHz sont visibles.");
        lv_obj_set_width(p, 226);
        lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
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
    lv_obj_t *t = topbar(w, "Wi-Fi", wifi_back_cb, NULL);
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
        lv_label_set_text_fmt(pw_msg, "%u caractères", (unsigned)n);
    } else {
        lv_obj_add_state(pw_go, LV_STATE_DISABLED);
        if (n) lv_label_set_text_fmt(pw_msg, "Encore %u caractère%s (8 minimum)", (unsigned)(8 - n), 8 - n > 1 ? "s" : "");
        else lv_label_set_text(pw_msg, "8 caractères minimum");
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
    lv_label_set_text(l, hide ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text_fmt(busy_lbl, "Connexion à %s…", pw_ssid);
    lv_obj_clear_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(busy);
    plat_wifi_connect(pw_ssid, lv_textarea_get_text(pw_ta));
}

static void open_pw(const char *ssid)
{
    snprintf(pw_ssid, sizeof pw_ssid, "%s", ssid);
    lv_label_set_text(pw_title, ssid);
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
}

static void offline_btn_cb(lv_event_t *e) { (void)e; open_wifi(); }
static void hint_hide(lv_timer_t *t) { (void)t; lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN); }

static void set_chip(const char *txt, lv_color_t bg)
{
    lv_label_set_text(chip, txt);
    lv_obj_set_style_bg_color(chip, bg, 0);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    n++;
    layout();
    if (selected >= 0) update_panel();
    if (!lv_obj_has_flag(list_panel, LV_OBJ_FLAG_HIDDEN) && n % 4 == 0 && !lv_obj_has_state(list_rows, LV_STATE_PRESSED)) render_list();

    char ssid[33], ip[16];
    int rssi;
    plat_net_state_t st = plat_wifi_info(ssid, ip, &rssi);
    int vis = 0;
    for (int i = 0; i < MAX_AC; i++) vis += S[i].used && S[i].visible;
    lv_label_set_text_fmt(count_lbl, LV_SYMBOL_LIST "  %d avion%s", vis, vis > 1 ? "s" : "");
    if (st != NET_UP) {
        set_chip("HORS LIGNE", C_ERR);
        lv_label_set_text(age_lbl, "");
        lv_obj_set_style_text_color(wifi_btn_lbl, C_ERR, 0);
        if (!plat_wifi_has_saved() || st == NET_FAILED) lv_obj_clear_flag(offline, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_color(wifi_btn_lbl, C_FG, 0);
        lv_obj_add_flag(offline, LV_OBJ_FLAG_HIDDEN);
        if (!last_ok) { set_chip("CHARGEMENT", C_ACCENT); lv_label_set_text(age_lbl, ""); }
        else {
            set_chip(api_down ? "API INJOIGNABLE" : "EN DIRECT", api_down ? C_ERR : C_OK);
            lv_label_set_text_fmt(age_lbl, "%s · il y a %u s", last_source, (unsigned)((plat_ms() - last_ok) / 1000));
        }
    }
    int h, m;
    if (plat_local_time(&h, &m)) lv_label_set_text_fmt(clock_lbl, "%02d:%02d", h, m);
}

static void events(lv_timer_t *t)
{
    (void)t;
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
            lv_label_set_text(pw_msg, "Connexion impossible : vérifiez le mot de passe.");
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
    lv_obj_t *bg = lv_img_create(map_obj);
    lv_img_set_src(bg, &img_map_bg);

    lv_obj_t *bar = box(scr, 0, 0, SCR_W, TOP_H, C_PANEL);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, C_LINE, 0);
    lv_obj_t *title = label(bar, &font_s16, C_FG, "Mulhouse – Bâle");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
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

    wifi_scr = build_wifi(scr);
    pw_scr = build_pw(scr);

    lv_timer_create(tick, 1000, NULL);
    lv_timer_create(events, 100, NULL);
    tick(NULL);
    adsb_start();
    if (!plat_wifi_has_saved()) open_wifi();
}

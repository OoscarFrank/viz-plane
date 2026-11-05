#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define MAX_AC 64

/* Map extent: same as the web simulator and tools/gen_assets.py */
#define LON0 6.93f
#define LON1 8.01f
#define LAT0 47.47f
#define LAT1 47.87f
#define SCR_W 800
#define TOP_H 40
#define MAP_H 440
#define CENTER_LAT 47.67
#define CENTER_LON 7.47
#define RADIUS_NM 30
#define MUL_LAT 47.7508f
#define MUL_LON 7.3359f

typedef enum { KIND_JET, KIND_LIGHT, KIND_HELI } ac_kind_t;

typedef struct {
    char hex[7];
    char call[9];
    char reg[12];
    char type[6];
    char desc[34];
    char squawk[5];
    float lat, lon, trk, gs;   /* degrees, degrees, degrees, knots */
    int alt, vs;               /* ft, ft/min */
    ac_kind_t kind;
    float seen_pos;            /* age of the position at the API, s */
    uint8_t db_flags;          /* readsb dbFlags: 1 = military */
    char emergency[10];        /* readsb emergency: general, lifeguard, minfuel, nordo, unlawful, downed */
} ac_t;

typedef struct {
    char from[5], to[5];
    char from_city[24], to_city[24];
    float from_lat, from_lon, to_lat, to_lon;
    char airline[32], flight[10];
} route_t;

/* adsb.c */
void adsb_start(void);
void adsb_request_route(const char *callsign, bool urgent);  /* LVGL lock held */
bool adsb_route_pending(const char *callsign);           /* lookup not answered yet; LVGL lock held */
const route_t *adsb_route(const ac_t *a);                /* NULL if unknown or implausible; LVGL lock held */

/* ui.c: called by adsb.c, LVGL lock held */
void ui_apply_aircraft(const ac_t *list, int n, const char *source);
void ui_fetch_failed(void);
void ui_route_ready(const char *callsign);
void ui_world_count(int n);                              /* aircraft tracked worldwide */

/* Assets */
LV_FONT_DECLARE(font_m12);
LV_FONT_DECLARE(font_m14);
LV_FONT_DECLARE(font_s16);
LV_FONT_DECLARE(font_s20);
LV_FONT_DECLARE(font_b24);
LV_IMG_DECLARE(img_map_bg);
LV_IMG_DECLARE(img_jet);
LV_IMG_DECLARE(img_jet_ol);
LV_IMG_DECLARE(img_light);
LV_IMG_DECLARE(img_light_ol);
LV_IMG_DECLARE(img_heli);
LV_IMG_DECLARE(img_heli_ol);

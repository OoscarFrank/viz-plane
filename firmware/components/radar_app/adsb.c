/* Data thread: ADS-B aircraft (adsb.lol, adsb.fi, airplanes.live) and routes (adsbdb).
 * Same logic as radar.py: fall back between providers, back off after a 429. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "plat.h"
#include "app_internal.h"
#include "assets/airports.h"

#define POLL_MS 10000
#define WORLD_MS 60000
#define BUF_CAP (160 * 1024)
#define ROUTE_CACHE 96       /* > MAX_AC: every aircraft in view keeps its route */
#define ROUTE_GAP_MS 250     /* spacing between adsbdb requests */

typedef struct { const char *name; const char *url; uint32_t pause_until; int strikes; } provider_t;

static provider_t providers[] = {
    {"adsb.lol", "https://api.adsb.lol/v2/point/47.67/7.47/30", 0, 0},
    {"adsb.fi", "https://opendata.adsb.fi/api/v2/lat/47.67/lon/7.47/dist/30", 0, 0},
    {"airplanes.live", "https://api.airplanes.live/v2/point/47.67/7.47/30", 0, 0},
};
#define NPROV (int)(sizeof providers / sizeof providers[0])

typedef struct { char call[9]; bool done; bool found; bool urgent; route_t r; uint32_t retry_at; } route_slot_t;
static route_slot_t routes[ROUTE_CACHE];    /* guarded by the LVGL lock */
static int route_next;

static char *buf;
static ac_t parsed[MAX_AC];

static void copy_str(char *dst, size_t cap, const cJSON *it)
{
    dst[0] = 0;
    if (!cJSON_IsString(it) || !it->valuestring) return;
    const char *s = it->valuestring;
    while (*s == ' ') s++;
    snprintf(dst, cap, "%s", s);
    for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = 0;
}

static double num(const cJSON *o, const char *k, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static int parse_aircraft(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "ac");
    if (!cJSON_IsArray(arr)) arr = cJSON_GetObjectItemCaseSensitive(root, "aircraft");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, arr) {
        if (n >= MAX_AC) break;
        const cJSON *alt = cJSON_GetObjectItemCaseSensitive(a, "alt_baro");
        if (cJSON_IsString(alt)) continue;                          /* "ground" */
        double lat = num(a, "lat", 999), lon = num(a, "lon", 999);
        if (lat > 90 || num(a, "seen_pos", 0) > 30) continue;
        ac_t *p = &parsed[n];
        memset(p, 0, sizeof *p);
        copy_str(p->hex, sizeof p->hex, cJSON_GetObjectItemCaseSensitive(a, "hex"));
        if (!p->hex[0]) continue;
        for (char *c = p->hex; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        copy_str(p->call, sizeof p->call, cJSON_GetObjectItemCaseSensitive(a, "flight"));
        copy_str(p->reg, sizeof p->reg, cJSON_GetObjectItemCaseSensitive(a, "r"));
        copy_str(p->type, sizeof p->type, cJSON_GetObjectItemCaseSensitive(a, "t"));
        copy_str(p->desc, sizeof p->desc, cJSON_GetObjectItemCaseSensitive(a, "desc"));
        copy_str(p->squawk, sizeof p->squawk, cJSON_GetObjectItemCaseSensitive(a, "squawk"));
        copy_str(p->emergency, sizeof p->emergency, cJSON_GetObjectItemCaseSensitive(a, "emergency"));
        p->db_flags = (uint8_t)num(a, "dbFlags", 0);
        if (!p->call[0]) snprintf(p->call, sizeof p->call, "%s", p->reg[0] ? p->reg : p->hex);
        p->lat = (float)lat; p->lon = (float)lon;
        p->alt = cJSON_IsNumber(alt) ? alt->valueint : (int)num(a, "alt_geom", 0);
        p->gs = (float)num(a, "gs", 0);
        p->trk = (float)num(a, "track", num(a, "true_heading", 0));
        p->vs = (int)num(a, "baro_rate", num(a, "geom_rate", 0));
        p->seen_pos = (float)num(a, "seen_pos", 0);
        char cat[4] = "";
        copy_str(cat, sizeof cat, cJSON_GetObjectItemCaseSensitive(a, "category"));
        p->kind = !strcmp(cat, "A7") ? KIND_HELI : (!strcmp(cat, "A1") || !strcmp(cat, "B1")) ? KIND_LIGHT : KIND_JET;
        n++;
    }
    cJSON_Delete(root);
    return n;
}

/* One attempt across the available providers. Returns the number of aircraft, or -1. */
static int fetch_once(const char **source)
{
    uint32_t now = plat_ms();
    for (int i = 0; i < NPROV; i++) {
        provider_t *pv = &providers[i];
        if ((int32_t)(pv->pause_until - now) > 0) continue;
        size_t len = 0;
        int retry = 0;
        int st = plat_http_get(pv->url, buf, BUF_CAP - 1, &len, &retry);
        if (st == 200) {
            buf[len] = 0;
            int n = parse_aircraft(buf);
            if (n >= 0) { pv->strikes = 0; *source = pv->name; return n; }
            printf("[radar] %s: unreadable response (%u bytes)\n", pv->name, (unsigned)len);
            pv->pause_until = now + 30000;
        } else if (st == 429) {
            pv->strikes++;
            int wait = retry > 0 ? retry : 60 << (pv->strikes - 1 > 3 ? 3 : pv->strikes - 1);
            if (wait > 600) wait = 600;
            printf("[radar] %s is rate limiting: pausing %d s\n", pv->name, wait);
            pv->pause_until = now + (uint32_t)wait * 1000;
        } else {
            printf("[radar] %s: error %d, pausing 30 s\n", pv->name, st);
            pv->pause_until = now + 30000;
        }
    }
    return -1;
}

static int cmp_icao(const void *k, const void *e) { return strncmp(k, ((const airport_t *)e)->icao, 4); }

/* ICAO code -> displayed code (IATA if any), city, position. False if unknown. */
static bool airport_fill(const char *icao, char *code, char *city, float *lat, float *lon)
{
    const airport_t *a = bsearch(icao, airports, airports_n, sizeof *airports, cmp_icao);
    if (!a) return false;
    snprintf(code, 5, "%s", a->iata[0] ? a->iata : a->icao);
    snprintf(city, 24, "%s", a->city);
    *lat = a->lat;
    *lon = a->lon;
    return true;
}

static void airport(cJSON *a, char *iata, char *city, float *lat, float *lon)
{
    char tmp[64];
    copy_str(tmp, sizeof tmp, cJSON_GetObjectItemCaseSensitive(a, "iata_code"));
    if (!tmp[0]) copy_str(tmp, sizeof tmp, cJSON_GetObjectItemCaseSensitive(a, "icao_code"));
    snprintf(iata, 5, "%s", tmp);
    copy_str(tmp, sizeof tmp, cJSON_GetObjectItemCaseSensitive(a, "municipality"));
    snprintf(city, 24, "%s", tmp);
    *lat = (float)num(a, "latitude", 0);
    *lon = (float)num(a, "longitude", 0);
}

/* Picks a pending route, the tapped aircraft first (lock held), then queries it (lock released).
 * Returns false when nothing is pending. */
static bool serve_one_route(void)
{
    char call[9] = "";
    int idx = -1;
    plat_lock(-1);
    for (int i = 0; i < ROUTE_CACHE; i++)
        if (routes[i].call[0] && !routes[i].done && (int32_t)(plat_ms() - routes[i].retry_at) >= 0 &&
            (idx < 0 || (routes[i].urgent && !routes[idx].urgent))) idx = i;
    if (idx >= 0) snprintf(call, sizeof call, "%s", routes[idx].call);
    plat_unlock();
    if (idx < 0) return false;

    /* 1. hexdb.io: route by callsign, kept up to date (adsbdb's are often stale) */
    char url[80];
    size_t len = 0;
    int retry = 0;
    route_t r = {0};
    bool found = false, done = true;
    uint32_t t0 = plat_ms();
    snprintf(url, sizeof url, "https://hexdb.io/api/v1/route/icao/%s", call);
    int st = plat_http_get(url, buf, BUF_CAP - 1, &len, &retry);
    if (st == 200) {
        buf[len] = 0;
        cJSON *root = cJSON_Parse(buf);
        char legs[64];
        copy_str(legs, sizeof legs, cJSON_GetObjectItemCaseSensitive(root, "route"));
        cJSON_Delete(root);
        /* "LFSB-EDDM", or several legs "A-B-C": keep the first and last airports */
        char *last = strrchr(legs, '-');
        if (last && last - legs >= 4) {
            legs[4] = 0;
            found = airport_fill(legs, r.from, r.from_city, &r.from_lat, &r.from_lon) &&
                    airport_fill(last + 1, r.to, r.to_city, &r.to_lat, &r.to_lon);
        }
    } else if (st != 404) {
        done = false;                                   /* transient error: retry later */
    }

    /* 2. adsbdb: airline name, and the route when hexdb has none */
    snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", call);
    int st2 = done ? plat_http_get(url, buf, BUF_CAP - 1, &len, &retry) : -1;
    if (st2 == 200) {
        buf[len] = 0;
        cJSON *root = cJSON_Parse(buf);
        cJSON *fr = root ? cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "response"), "flightroute") : NULL;
        if (fr) {
            if (!found) {
                airport(cJSON_GetObjectItemCaseSensitive(fr, "origin"), r.from, r.from_city, &r.from_lat, &r.from_lon);
                airport(cJSON_GetObjectItemCaseSensitive(fr, "destination"), r.to, r.to_city, &r.to_lat, &r.to_lon);
                found = r.from[0] && r.to[0];
            }
            copy_str(r.airline, sizeof r.airline, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(fr, "airline"), "name"));
            copy_str(r.flight, sizeof r.flight, cJSON_GetObjectItemCaseSensitive(fr, "callsign_iata"));
        }
        cJSON_Delete(root);
    }
    printf("[radar] route %s: hexdb %d, adsbdb %d -> %s-%s in %u ms\n", call, st, st2,
           found ? r.from : "?", found ? r.to : "?", (unsigned)(plat_ms() - t0));

    plat_lock(-1);
    if (!strcmp(routes[idx].call, call)) {
        routes[idx].done = done;
        routes[idx].found = found;
        routes[idx].r = r;
        routes[idx].retry_at = plat_ms() + 30000;
        if (done) ui_route_ready(call);
    }
    plat_unlock();
    return true;
}

void adsb_request_route(const char *callsign, bool urgent)
{
    if (!callsign[0] || strlen(callsign) > 8) return;
    for (int i = 0; i < ROUTE_CACHE; i++)
        if (!strcmp(routes[i].call, callsign)) {
            if (urgent && !routes[i].done) { routes[i].urgent = true; routes[i].retry_at = plat_ms(); }
            return;
        }
    route_slot_t *s = &routes[route_next];
    route_next = (route_next + 1) % ROUTE_CACHE;
    memset(s, 0, sizeof *s);
    snprintf(s->call, sizeof s->call, "%s", callsign);
    s->urgent = urgent;
    s->retry_at = plat_ms();
}

#define RAD(d) ((d) * 0.017453293f)

static float gc_km(float lat1, float lon1, float lat2, float lon2)
{
    float a = sinf(RAD(lat2 - lat1) / 2), b = sinf(RAD(lon2 - lon1) / 2);
    return 12742.0f * asinf(sqrtf(a * a + cosf(RAD(lat1)) * cosf(RAD(lat2)) * b * b));
}

static float bearing(float lat1, float lon1, float lat2, float lon2)
{
    float y = sinf(RAD(lon2 - lon1)) * cosf(RAD(lat2));
    float x = cosf(RAD(lat1)) * sinf(RAD(lat2)) - sinf(RAD(lat1)) * cosf(RAD(lat2)) * cosf(RAD(lon2 - lon1));
    return atan2f(y, x) / 0.017453293f;
}

/* Callsign route databases are often stale (callsigns get reused for other routes).
 * Keep a route only if the aircraft lies on it and flies toward the destination. */
static bool route_plausible(const route_t *r, const ac_t *a)
{
    float da = gc_km(r->from_lat, r->from_lon, a->lat, a->lon);
    float db = gc_km(a->lat, a->lon, r->to_lat, r->to_lon);
    float d = gc_km(r->from_lat, r->from_lon, r->to_lat, r->to_lon);
    if (d < 1 || da + db > d * 1.15f + 30) return false;          /* off the great circle */
    if (da < 100 || db < 100) return true;                        /* climbing out or on approach: any heading */
    float dh = fabsf(fmodf(bearing(a->lat, a->lon, r->to_lat, r->to_lon) - a->trk + 540.0f, 360.0f) - 180.0f);
    return dh < 100;                                              /* not flying away from the destination */
}

bool adsb_route_pending(const char *callsign)
{
    for (int i = 0; i < ROUTE_CACHE; i++)
        if (!strcmp(routes[i].call, callsign)) return !routes[i].done;
    return callsign[0] != 0;
}

#define BSL_LAT 47.5896f
#define BSL_LON 7.5299f

/* Departures from / arrivals to EuroAirport, read off the flight itself:
 * low, close, climbing away from the airport or descending toward it. */
static int bsl_movement(const ac_t *a)       /* +1 departure, -1 arrival, 0 neither */
{
    float d = gc_km(BSL_LAT, BSL_LON, a->lat, a->lon);
    if (a->alt <= 0 || a->alt > 15000 || d > 60) return 0;
    if (d < 3) return a->vs > 300 ? 1 : a->vs < -300 ? -1 : 0;
    float away = bearing(BSL_LAT, BSL_LON, a->lat, a->lon);
    float dh = fabsf(fmodf(away - a->trk + 540.0f, 360.0f) - 180.0f);
    if (a->vs > 500 && dh < 70) return 1;
    if (a->vs < -500 && dh > 110) return -1;
    return 0;
}

const route_t *adsb_route(const ac_t *a)
{
    const route_t *db = NULL;
    for (int i = 0; i < ROUTE_CACHE; i++)
        if (routes[i].done && routes[i].found && !strcmp(routes[i].call, a->call)) { db = &routes[i].r; break; }
    int mv = bsl_movement(a);
    if (db && route_plausible(db, a) && (mv == 0 || !strcmp(mv > 0 ? db->from : db->to, "BSL"))) return db;
    if (mv == 0) return NULL;
    /* the database is wrong or silent: keep the airport we are sure of, the other end is unknown */
    static route_t r;
    memset(&r, 0, sizeof r);
    if (db) { memcpy(r.airline, db->airline, sizeof r.airline); memcpy(r.flight, db->flight, sizeof r.flight); }
    snprintf(mv > 0 ? r.from : r.to, 5, "BSL");
    snprintf(mv > 0 ? r.from_city : r.to_city, 24, "Bâle-Mulhouse");
    snprintf(mv > 0 ? r.to : r.from, 5, "?");
    if (mv > 0) { r.from_lat = BSL_LAT; r.from_lon = BSL_LON; } else { r.to_lat = BSL_LAT; r.to_lon = BSL_LON; }
    return &r;
}

/* Aircraft tracked worldwide by the adsb.fi network */
static void fetch_world(void)
{
    size_t len = 0;
    int retry = 0;
    if (plat_http_get("https://api.adsb.fi/v1/stats", buf, BUF_CAP - 1, &len, &retry) != 200) return;
    buf[len] = 0;
    cJSON *root = cJSON_Parse(buf);
    int n = (int)num(root, "totalAircraft", 0);
    cJSON_Delete(root);
    if (n <= 0) return;
    plat_lock(-1);
    ui_world_count(n);
    plat_unlock();
}

volatile bool app_sleeping;
static volatile bool wake_now;
void adsb_wake(void) { wake_now = true; }

static void task(void *arg)
{
    (void)arg;
    uint32_t next = 0, next_world = 0;
    for (;;) {
        if (app_sleeping) { plat_sleep_ms(500); continue; }
        if (wake_now) { wake_now = false; next = next_world = plat_ms(); }
        char ssid[33], ip[16];
        int rssi;
        if (plat_wifi_info(ssid, ip, &rssi) != NET_UP) { plat_sleep_ms(500); continue; }
        if ((int32_t)(plat_ms() - next) >= 0) {
            const char *src = NULL;
            int n = fetch_once(&src);
            plat_lock(-1);
            if (n >= 0) {
                ui_apply_aircraft(parsed, n, src);
                for (int i = 0; i < n; i++) adsb_request_route(parsed[i].call, false);   /* prefetch */
            } else {
                ui_fetch_failed();
            }
            plat_unlock();
            next = plat_ms() + POLL_MS;
        }
        if ((int32_t)(plat_ms() - next_world) >= 0) {
            fetch_world();
            next_world = plat_ms() + WORLD_MS;
        }
        plat_sleep_ms(serve_one_route() ? ROUTE_GAP_MS : 100);
    }
}

void adsb_start(void)
{
    buf = plat_big_alloc(BUF_CAP);
    plat_thread(task, NULL, "adsb", 12 * 1024);
}

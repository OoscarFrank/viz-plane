/* Simulator platform: pthreads, libcurl, fake Wi-Fi. */
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "plat.h"
#include "sim_tick.h"

static pthread_mutex_t lock;
static struct timespec t0;
static plat_net_state_t net = NET_UP;
static char ssid_cur[33] = "Maison_2G";
static bool saved = true;
static char *demo_json;
static size_t demo_len;

void sim_plat_init(bool wifi_setup, const char *demo_file)
{
    if (demo_file) {
        FILE *f = fopen(demo_file, "rb");
        if (f) { demo_json = malloc(1 << 20); demo_len = fread(demo_json, 1, (1 << 20), f); fclose(f); }
        else fprintf(stderr, "demo file not found: %s\n", demo_file);
    }
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &a);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (wifi_setup) { saved = false; net = NET_DOWN; ssid_cur[0] = 0; }
}

uint32_t sim_tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

uint32_t plat_ms(void) { return sim_tick_ms(); }
void plat_sleep_ms(uint32_t ms) { usleep(ms * 1000); }
void *plat_big_alloc(size_t n) { return malloc(n); }
void plat_big_free(void *p) { free(p); }
bool plat_lock(int timeout_ms) { (void)timeout_ms; return pthread_mutex_lock(&lock) == 0; }
void plat_unlock(void) { pthread_mutex_unlock(&lock); }

bool plat_local_time(int *h, int *m)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    *h = t.tm_hour; *m = t.tm_min;
    return true;
}

typedef struct { void (*fn)(void *); void *arg; } tstart_t;
static void *trampoline(void *p) { tstart_t s = *(tstart_t *)p; free(p); s.fn(s.arg); return NULL; }

void plat_thread(void (*fn)(void *), void *arg, const char *name, uint32_t stack)
{
    tstart_t *s = malloc(sizeof *s);
    s->fn = fn; s->arg = arg;
    pthread_t th;
    pthread_create(&th, NULL, trampoline, s);
    pthread_detach(th);
}

typedef struct { char *buf; size_t cap, len; int retry_after; } ctx_t;

static size_t on_data(char *p, size_t sz, size_t n, void *u)
{
    ctx_t *c = u;
    size_t k = sz * n, room = c->cap - c->len;
    memcpy(c->buf + c->len, p, k < room ? k : room);
    c->len += k < room ? k : room;
    return k;
}

static size_t on_header(char *p, size_t sz, size_t n, void *u)
{
    ctx_t *c = u;
    if (sz * n > 12 && !strncasecmp(p, "Retry-After:", 12)) c->retry_after = atoi(p + 12);
    return sz * n;
}

int plat_http_get(const char *url, char *buf, size_t cap, size_t *len, int *retry_after)
{
    if (demo_json) {                                   /* demo mode: frozen traffic, one known route */
        if (!strstr(url, "adsbdb")) { size_t n = demo_len < cap ? demo_len : cap; memcpy(buf, demo_json, n); *len = n; *retry_after = 0; return 200; }
        if (strstr(url, "EZS1477")) {
            const char *r = "{\"response\":{\"flightroute\":{\"callsign_iata\":\"DS1477\",\"airline\":{\"name\":\"easyJet Switzerland\"},"
                            "\"origin\":{\"iata_code\":\"LIS\",\"municipality\":\"Lisbonne\",\"latitude\":38.77,\"longitude\":-9.13},"
                            "\"destination\":{\"iata_code\":\"BSL\",\"municipality\":\"Bâle-Mulhouse\",\"latitude\":47.59,\"longitude\":7.53}}}}";
            *len = strlen(r); memcpy(buf, r, *len); *retry_after = 0; return 200;
        }
        *len = 0; *retry_after = 0; return 404;
    }
    ctx_t c = {buf, cap, 0, 0};
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "radar-mulhouse-bale/1.0");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &c);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    long st = -1;
    if (curl_easy_perform(h) == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &st);
    curl_easy_cleanup(h);
    *len = c.len;
    *retry_after = c.retry_after;
    return (int)st;
}

/* ---------- fake Wi-Fi ---------- */
static const plat_ap_t FAKE[] = {{"Maison_2G", -54, true}, {"Freebox-7A3C1E", -61, true}, {"Livebox-21F0", -68, true},
                                 {"SFR_BOX_8842", -73, true}, {"Invités", -82, false}};
static char pending_ssid[33], pending_pw[64];

static void scan_job(void *a) { plat_sleep_ms(1000); app_wifi_scan_done(FAKE, 5); }
static void connect_job(void *a)
{
    plat_sleep_ms(1500);
    bool ok = strlen(pending_pw) >= 8 || !strcmp(pending_ssid, "Invités");
    if (ok) { snprintf(ssid_cur, sizeof ssid_cur, "%s", pending_ssid); saved = true; net = NET_UP; app_wifi_event(NET_UP); }
    else { net = NET_FAILED; app_wifi_event(NET_FAILED); }
}

void plat_wifi_scan(void) { plat_thread(scan_job, NULL, "scan", 0); }
void plat_wifi_connect(const char *ssid, const char *pw)
{
    snprintf(pending_ssid, sizeof pending_ssid, "%s", ssid);
    snprintf(pending_pw, sizeof pending_pw, "%s", pw);
    net = NET_CONNECTING;
    plat_thread(connect_job, NULL, "connect", 0);
}
void plat_wifi_forget(void) { saved = false; net = NET_DOWN; ssid_cur[0] = 0; }
bool plat_wifi_has_saved(void) { return saved; }
plat_net_state_t plat_wifi_info(char ssid[33], char ip[16], int *rssi)
{
    snprintf(ssid, 33, "%s", ssid_cur);
    snprintf(ip, 16, "%s", net == NET_UP ? "192.168.1.42" : "");
    *rssi = net == NET_UP ? -54 : 0;
    return net;
}

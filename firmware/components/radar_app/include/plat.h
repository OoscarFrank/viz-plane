/* Interface between the (portable) app and the platform: ESP32 or Mac simulator. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char ssid[33];
    int8_t rssi;     /* dBm */
    bool secure;
} plat_ap_t;

typedef enum { NET_DOWN, NET_CONNECTING, NET_UP, NET_FAILED } plat_net_state_t;

/* Time, memory, tasks */
uint32_t plat_ms(void);
void plat_sleep_ms(uint32_t ms);
bool plat_local_time(int *hour, int *min);          /* false until the clock is synced */
void *plat_big_alloc(size_t n);                      /* large buffers (PSRAM on the ESP32) */
void plat_big_free(void *p);
void plat_thread(void (*fn)(void *), void *arg, const char *name, uint32_t stack_bytes);

/* LVGL lock (recursive): take it to touch the UI from another thread */
bool plat_lock(int timeout_ms);
void plat_unlock(void);

/* Blocking HTTP(S) GET. Returns the HTTP status, or < 0 on a network error.
 * *retry_after receives the Retry-After header in seconds (0 if absent). */
int plat_http_get(const char *url, char *buf, size_t cap, size_t *len, int *retry_after);

/* Wi-Fi: asynchronous calls, results through the app_* functions below (from any thread) */
void plat_wifi_scan(void);
void plat_wifi_connect(const char *ssid, const char *password);
void plat_wifi_forget(void);
bool plat_wifi_has_saved(void);
plat_net_state_t plat_wifi_info(char ssid[33], char ip[16], int *rssi);

/* Implemented by the app */
void app_wifi_scan_done(const plat_ap_t *aps, int n);
void app_wifi_event(plat_net_state_t state);

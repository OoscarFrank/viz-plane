/* ESP32-S3 platform: time, memory, tasks, Wi-Fi, HTTPS, clock. */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "plat.h"
#include "plat_esp.h"

static const char *TAG = "plat";
static esp_netif_t *sta;
static volatile plat_net_state_t net = NET_DOWN;
static volatile bool user_connect;   /* connection started from the Wi-Fi screen */
static int retries;
static volatile bool scanning;      /* auto-reconnect paused: the driver refuses to scan while connecting */
static bool sntp_started;

uint32_t plat_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
void plat_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
void *plat_big_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void plat_big_free(void *p) { heap_caps_free(p); }
bool plat_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void plat_unlock(void) { lvgl_port_unlock(); }

void plat_thread(void (*fn)(void *), void *arg, const char *name, uint32_t stack)
{
    xTaskCreatePinnedToCore(fn, name, stack, arg, 3, NULL, 0);   /* LVGL runs on core 1 */
}

bool plat_local_date(int *wday, int *mday, int *mon)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 124) return false;
    *wday = t.tm_wday;
    *mday = t.tm_mday;
    *mon = t.tm_mon;
    return true;
}

bool plat_local_time(int *h, int *m)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 124) return false;
    *h = t.tm_hour;
    *m = t.tm_min;
    return true;
}

/* ---------- HTTP ---------- */
typedef struct { char *buf; size_t cap, len; int retry_after; } http_ctx_t;

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    http_ctx_t *c = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Retry-After")) c->retry_after = atoi(e->header_value);
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        size_t n = e->data_len;
        if (c->len + n > c->cap) n = c->cap - c->len;
        memcpy(c->buf + c->len, e->data, n);
        c->len += n;
    }
    return ESP_OK;
}

/* One kept-alive connection per host: a TLS handshake costs ~2 s on the ESP32.
 * Only the data thread makes requests, so no locking. */
#define HTTP_CONNS 5                    /* adsb.lol (or fallback), hexdb, adsbdb, adsb.fi stats */
static struct { char host[48]; esp_http_client_handle_t h; } conns[HTTP_CONNS];
static int conn_next;

static esp_http_client_handle_t conn_for(const char *url, http_ctx_t *c)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    char host[48];
    size_t n = strcspn(p, "/");
    snprintf(host, sizeof host, "%.*s", (int)(n < sizeof host ? n : sizeof host - 1), p);
    for (int i = 0; i < HTTP_CONNS; i++)
        if (conns[i].h && !strcmp(conns[i].host, host)) {
            esp_http_client_set_url(conns[i].h, url);
            esp_http_client_set_user_data(conns[i].h, c);
            return conns[i].h;
        }
    int i = conn_next;
    conn_next = (conn_next + 1) % HTTP_CONNS;
    if (conns[i].h) esp_http_client_cleanup(conns[i].h);
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_ev,
        .user_data = c,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .user_agent = "radar-mulhouse-bale/1.0",
        .buffer_size = 2048,
        .keep_alive_enable = true,
    };
    conns[i].h = esp_http_client_init(&cfg);
    snprintf(conns[i].host, sizeof conns[i].host, "%s", host);
    esp_http_client_set_header(conns[i].h, "Accept", "application/json");
    return conns[i].h;
}

static void conn_drop(esp_http_client_handle_t h)
{
    for (int i = 0; i < HTTP_CONNS; i++)
        if (conns[i].h == h) { esp_http_client_cleanup(h); conns[i].h = NULL; conns[i].host[0] = 0; }
}

int plat_http_get(const char *url, char *buf, size_t cap, size_t *len, int *retry_after)
{
    http_ctx_t c = {.buf = buf, .cap = cap};
    esp_http_client_handle_t h = conn_for(url, &c);
    esp_err_t err = esp_http_client_perform(h);
    if (err != ESP_OK) {                        /* the server may have closed the idle connection: retry once, fresh */
        conn_drop(h);
        c = (http_ctx_t){.buf = buf, .cap = cap};
        h = conn_for(url, &c);
        err = esp_http_client_perform(h);
    }
    int st = err == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s : %s", url, esp_err_to_name(err));
        conn_drop(h);
    }
    *len = c.len;
    *retry_after = c.retry_after;
    return st;
}

/* ---------- Wi-Fi ---------- */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t c = {0};
        esp_wifi_get_config(WIFI_IF_STA, &c);
        ESP_LOGI(TAG, "saved network: \"%s\"", (char *)c.sta.ssid);
        if (c.sta.ssid[0]) { net = NET_CONNECTING; esp_wifi_connect(); }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool auth = d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT || d->reason == WIFI_REASON_NO_AP_FOUND;
        ESP_LOGI(TAG, "disconnected, reason %d", d->reason);
        if (scanning) {
            net = NET_DOWN;
        } else if (user_connect && (auth || ++retries > 3)) {
            user_connect = false;
            net = NET_FAILED;
            app_wifi_event(NET_FAILED);
        } else if (plat_wifi_has_saved()) {
            net = NET_CONNECTING;
            vTaskDelay(pdMS_TO_TICKS(user_connect ? 500 : 5000));
            esp_wifi_connect();
        } else {
            net = NET_DOWN;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        user_connect = false;
        retries = 0;
        net = NET_UP;
        app_wifi_event(NET_UP);
        if (!sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
            sntp_started = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 32;
        wifi_ap_record_t *rec = calloc(n, sizeof *rec);
        plat_ap_t *aps = calloc(24, sizeof *aps);
        int m = 0;
        esp_err_t err = rec && aps ? esp_wifi_scan_get_ap_records(&n, rec) : ESP_ERR_NO_MEM;
        ESP_LOGI(TAG, "scan done: %s, %u access points", esp_err_to_name(err), err == ESP_OK ? n : 0);
        if (err == ESP_OK) {
            for (int i = 0; i < n && m < 24; i++) {          /* already sorted by decreasing signal */
                ESP_LOGI(TAG, "  ch%2d %4d dBm  %s", rec[i].primary, rec[i].rssi, (char *)rec[i].ssid);
                if (!rec[i].ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < m && !dup; j++) dup = !strcmp(aps[j].ssid, (char *)rec[i].ssid);
                if (dup) continue;
                snprintf(aps[m].ssid, sizeof aps[m].ssid, "%s", (char *)rec[i].ssid);
                aps[m].rssi = rec[i].rssi;
                aps[m].secure = rec[i].authmode != WIFI_AUTH_OPEN;
                m++;
            }
        }
        app_wifi_scan_done(aps, m);
        free(rec);
        free(aps);
        if (scanning) {
            scanning = false;
            if (net != NET_UP && !user_connect && plat_wifi_has_saved()) { net = NET_CONNECTING; esp_wifi_connect(); }
        }
    }
}

bool plat_wifi_has_saved(void)
{
    wifi_config_t c = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0];
}

void plat_wifi_scan(void)
{
    wifi_scan_config_t sc = {.scan_time.active = {.min = 100, .max = 300}};
    if (net != NET_UP) {
        scanning = true;
        esp_wifi_disconnect();
    }
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    ESP_LOGI(TAG, "scan start: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        scanning = false;
        app_wifi_scan_done(NULL, 0);
    }
}

void plat_wifi_connect(const char *ssid, const char *password)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof c.sta.ssid, "%s", ssid);
    snprintf((char *)c.sta.password, sizeof c.sta.password, "%s", password);
    c.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    user_connect = true;
    scanning = false;
    retries = 0;
    net = NET_CONNECTING;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &c);       /* saved to flash (NVS) */
    esp_wifi_connect();
}

void plat_wifi_forget(void)
{
    wifi_config_t c = {0};
    user_connect = false;
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_disconnect();
    net = NET_DOWN;
}

plat_net_state_t plat_wifi_info(char ssid[33], char ip[16], int *rssi)
{
    wifi_config_t c = {0};
    ssid[0] = ip[0] = 0;
    *rssi = 0;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) snprintf(ssid, 33, "%s", (char *)c.sta.ssid);
    if (net == NET_UP) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(sta, &info) == ESP_OK) snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    }
    return net;
}

static void *ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void plat_esp_init(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    cJSON_Hooks hooks = {.malloc_fn = ps_malloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&hooks);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}


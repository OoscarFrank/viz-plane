/* Over-the-air updates from the GitHub releases of the project.
 *
 * Once a night (and shortly after boot) the latest release is read from the GitHub API.
 * If its tag (vX.Y.Z) is newer than the running firmware, its radar.bin asset is
 * written to the other OTA slot and the board restarts on it. The new firmware is
 * marked valid once it has been online for a minute; if it crashes before that,
 * the bootloader rolls back to the previous one.
 * Development builds (version not of the form vX.Y.Z) never update themselves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "plat.h"
#include "ota.h"

#define RELEASE_API "https://api.github.com/repos/OoscarFrank/viz-plane/releases/latest"
#define ASSET "radar.bin"
#define CHECK_HOUR 4                      /* local time of the nightly check */

static const char *TAG = "ota";
static volatile bool check_now;

const char *plat_version(void) { return esp_app_get_description()->version; }
void plat_update_check(void) { check_now = true; }

static bool parse_ver(const char *s, int v[3])
{
    return s && sscanf(s, "v%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

static bool newer(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

/* Reads the latest release: its tag and the download URL of radar.bin */
static bool latest_release(char *tag, size_t tag_cap, char **url)
{
    const size_t cap = 64 * 1024;
    char *buf = plat_big_alloc(cap);
    if (!buf) return false;
    /* own client: plat_http_get keeps connections for the data thread only */
    esp_http_client_config_t cfg = {
        .url = RELEASE_API,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "viz-plane-ota",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
    size_t len = 0;
    int st = -1;
    if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
        st = esp_http_client_get_status_code(h);
        int n;
        while (len < cap - 1 && (n = esp_http_client_read(h, buf + len, cap - 1 - len)) > 0) len += n;
    }
    esp_http_client_cleanup(h);
    bool ok = false;
    if (st == 200) {
        buf[len] = 0;
        cJSON *root = cJSON_Parse(buf);
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
        const cJSON *a;
        cJSON_ArrayForEach(a, assets) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
            if (cJSON_IsString(name) && !strcmp(name->valuestring, ASSET) && cJSON_IsString(u) && cJSON_IsString(t)) {
                snprintf(tag, tag_cap, "%s", t->valuestring);
                *url = strdup(u->valuestring);
                ok = *url != NULL;
                break;
            }
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "release check: HTTP %d", st);
    }
    plat_big_free(buf);
    return ok;
}

static void check_and_update(bool by_user)
{
    int cur[3], rel[3];
    if (!parse_ver(plat_version(), cur)) {
        ESP_LOGI(TAG, "development build %s: no automatic update", plat_version());
        if (by_user) app_update_status("Version de développement : pas de mise à jour");
        return;
    }
    char tag[32];
    char *url = NULL;
    if (!latest_release(tag, sizeof tag, &url)) {
        if (by_user) app_update_status("Impossible de joindre GitHub");
        return;
    }
    if (!parse_ver(tag, rel) || !newer(rel, cur)) {
        ESP_LOGI(TAG, "up to date (%s, latest %s)", plat_version(), tag);
        if (by_user) app_update_status("L'écran est à jour");
        free(url);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "Mise à jour vers %s…", tag);
    app_update_status(msg);
    ESP_LOGI(TAG, "updating %s -> %s from %s", plat_version(), tag, url);
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,               /* the redirect to GitHub's storage has a long URL */
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_err_t err = esp_https_ota(&cfg);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update written, restarting");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    app_update_status("La mise à jour a échoué");
}

static void task(void *arg)
{
    (void)arg;
    char ssid[33], ip[16];
    int rssi;
    /* first boot of a new firmware: keep it once it has run online for a minute */
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        while (plat_wifi_info(ssid, ip, &rssi) != NET_UP) vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(60000));
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware %s marked valid", plat_version());
    }

    uint32_t boot_check = plat_ms() + 3 * 60 * 1000;   /* once shortly after boot */
    int last_day = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (plat_wifi_info(ssid, ip, &rssi) != NET_UP) continue;
        bool by_user = check_now;
        int h, m, wd, md, mo;
        bool nightly = plat_local_time(&h, &m) && plat_local_date(&wd, &md, &mo) && h == CHECK_HOUR && md != last_day;
        bool at_boot = boot_check && (int32_t)(plat_ms() - boot_check) >= 0;
        if (!by_user && !nightly && !at_boot) continue;
        check_now = false;
        if (nightly) last_day = md;
        if (at_boot) boot_check = 0;
        check_and_update(by_user);
    }
}

void ota_start(void)
{
    xTaskCreatePinnedToCore(task, "ota", 8192, NULL, 2, NULL, 0);
}

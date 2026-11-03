/* Mulhouse-Basel radar: ESP32-S3 entry point. */
#include "board.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lvgl_port.h"
#include "plat_esp.h"
#include "radar_app.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init());          /* display, touch, LVGL */
    plat_esp_init();                        /* Wi-Fi, clock */
    if (lvgl_port_lock(-1)) {
        radar_app_start();
        lvgl_port_unlock();
    }
    board_backlight_on();
    ESP_LOGI("radar", "started");
}

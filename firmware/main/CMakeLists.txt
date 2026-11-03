idf_component_register(
    SRCS "main.c" "board.c" "lvgl_port.c" "plat_esp.c"
    INCLUDE_DIRS "."
    REQUIRES radar_app lvgl__lvgl espressif__esp_lcd_touch_gt911 esp_lcd esp_wifi esp_netif esp_http_client
             esp-tls nvs_flash json esp_timer driver mbedtls)

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

#pragma once
#include "esp_err.h"

esp_err_t board_init(void);
esp_err_t board_backlight_on(void);
esp_err_t board_backlight_off(void);

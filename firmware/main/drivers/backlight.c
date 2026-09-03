#include "backlight.h"

#include "esp_log.h"

static const char *TAG = "driver_backlight";
static uint8_t s_current_brightness = 80;

esp_err_t backlight_init(void)
{
    ESP_LOGI(TAG, "背光驱动占位初始化，当前预设: %u%%", s_current_brightness);
    return ESP_OK;
}

esp_err_t backlight_set(uint8_t brightness_pct)
{
    if (brightness_pct > 100) {
        brightness_pct = 100;
    }
    s_current_brightness = brightness_pct;
    ESP_LOGI(TAG, "设置背光亮度占位值: %u%%", s_current_brightness);
    return ESP_OK;
}

uint8_t backlight_get(void)
{
    return s_current_brightness;
}

#include "battery.h"
#include "esp_log.h"

static const char *TAG = "driver_battery";
static uint32_t s_voltage_mv = 4120;
static uint8_t s_percentage = 88;
static bool s_is_charging = false;

esp_err_t battery_init(void)
{
    ESP_LOGI(TAG, "电池电量采样驱动初始化完成");
    return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
    return s_voltage_mv;
}

uint8_t battery_get_percentage(void)
{
    return s_percentage;
}

bool battery_is_charging(void)
{
    return s_is_charging;
}

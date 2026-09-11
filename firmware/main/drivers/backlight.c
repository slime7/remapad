#include "backlight.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

/* 背光引脚 BL=GPIO15 不是自动点亮，必须显式驱动；PWM 频率选在 25 kHz
 * 以避开可闻噪声范围。 */
#define REMAPAD_BACKLIGHT_GPIO 15
#define REMAPAD_BACKLIGHT_LEDC_FREQ_HZ 25000
#define REMAPAD_BACKLIGHT_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define REMAPAD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define REMAPAD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define REMAPAD_BACKLIGHT_DEFAULT_PCT 80

static const char *TAG = "driver_backlight";
static uint8_t s_current_brightness = 0;

esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = REMAPAD_BACKLIGHT_LEDC_RESOLUTION,
        .timer_num = REMAPAD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = REMAPAD_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "config LEDC timer failed");

    const ledc_channel_config_t channel_config = {
        .gpio_num = REMAPAD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = REMAPAD_BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = REMAPAD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "config LEDC channel failed");

    s_current_brightness = 0;
    return ESP_OK;
}

esp_err_t backlight_set(uint8_t brightness_pct)
{
    if (brightness_pct > 100) {
        brightness_pct = 100;
    }
    const uint32_t max_duty = (1U << REMAPAD_BACKLIGHT_LEDC_RESOLUTION) - 1U;
    const uint32_t duty = max_duty * brightness_pct / 100U;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, REMAPAD_BACKLIGHT_LEDC_CHANNEL, duty),
        TAG, "set LEDC duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(LEDC_LOW_SPEED_MODE, REMAPAD_BACKLIGHT_LEDC_CHANNEL),
        TAG, "update LEDC duty failed");
    s_current_brightness = brightness_pct;
    return ESP_OK;
}

uint8_t backlight_get(void)
{
    return s_current_brightness;
}

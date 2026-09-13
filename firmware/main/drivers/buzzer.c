#include "buzzer.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

static const char *TAG = "driver_buzzer";

/* 蜂鸣器为无源器件，需要 tone 驱动：LEDC 通道输出方波，鸣叫结束由
 * esp_timer 一次性回调拉低占空比（非阻塞，不占用调用任务）。 */
#define BUZZER_GPIO GPIO_NUM_42
#define BUZZER_LEDC_FREQ_HZ 4000
#define BUZZER_LEDC_DUTY 512 /* 10-bit 分辨率下的 50% */

static esp_timer_handle_t s_stop_timer;
static bool s_started;

static void stop_timer_cb(void *arg)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_started = false;
}

esp_err_t buzzer_init(void)
{
    const ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = BUZZER_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        return err;
    }
    const ledc_channel_config_t ch_conf = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        return err;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = stop_timer_cb,
        .name = "buzzer_stop",
    };
    err = esp_timer_create(&timer_args, &s_stop_timer);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "buzzer ready on GPIO%d (%dHz)", BUZZER_GPIO, BUZZER_LEDC_FREQ_HZ);
    return ESP_OK;
}

void buzzer_beep(uint32_t on_ms)
{
    if (s_stop_timer == NULL) {
        return;
    }
    if (on_ms == 0 || on_ms > 1000) {
        on_ms = 120;
    }
    esp_timer_stop(s_stop_timer);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, BUZZER_LEDC_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_started = true;
    esp_timer_start_once(s_stop_timer, (int64_t)on_ms * 1000LL);
}

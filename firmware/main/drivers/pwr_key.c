#include "pwr_key.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "buzzer.h"

/* PWR 键经电源功能电路接 SYS_OUT（GPIO40）；板上应有外部上拉，内部上拉
 * 仅作悬空兜底。SYS_EN（GPIO41）是电源保持脚：电池供电时按键松开后系统
 * 靠它维持，锁存见 pwr_key_power_hold；拉低即软件关机，当前没有入口。 */
#define PWR_KEY_GPIO GPIO_NUM_40
#define PWR_KEY_HOLD_GPIO GPIO_NUM_41
#define PWR_KEY_POLL_MS 10
#define PWR_KEY_SHORT_MAX_US (600 * 1000LL)
#define PWR_KEY_LONG_MIN_US (3 * 1000000LL)
#define PWR_KEY_LONG_MAX_US (6 * 1000000LL)

static const char *TAG = "driver_pwrkey";

typedef enum {
    PWR_IDLE = 0,
    PWR_DOWN,
    PWR_IGNORE, /* 超长按住：等待释放，不再产生事件。 */
} pwr_state_t;

static pwr_key_fn s_callback;
static void *s_user;

static void pwr_key_task(void *param)
{
    pwr_state_t state = PWR_IDLE;
    int64_t pressed_at_us = 0;
    bool long_hint_beeped = false;

    ESP_LOGI(TAG, "pwr key polling on GPIO%d", PWR_KEY_GPIO);
    for (;;) {
        const bool level_low = gpio_get_level(PWR_KEY_GPIO) == 0;
        const int64_t now = esp_timer_get_time();
        switch (state) {
        case PWR_IDLE:
            if (level_low) {
                pressed_at_us = now;
                long_hint_beeped = false;
                state = PWR_DOWN;
            }
            break;
        case PWR_DOWN:
            if (!level_low) {
                const int64_t held = now - pressed_at_us;
                if (held >= PWR_KEY_LONG_MIN_US && held <= PWR_KEY_LONG_MAX_US) {
                    ESP_LOGI(TAG, "long press %lld ms", (long long)(held / 1000LL));
                    s_callback(PWR_KEY_LONG, s_user);
                } else if (held < PWR_KEY_SHORT_MAX_US) {
                    ESP_LOGI(TAG, "short press %lld ms", (long long)(held / 1000LL));
                    s_callback(PWR_KEY_SHORT, s_user);
                } else {
                    ESP_LOGI(TAG, "press %lld ms ignored (no event)",
                             (long long)(held / 1000LL));
                }
                state = PWR_IDLE;
            } else if (now - pressed_at_us > PWR_KEY_LONG_MAX_US) {
                /* 按住超出长按窗：进入忽略态，避免在未知硬件切电阈值边缘
                 * 触发软件动作。 */
                state = PWR_IGNORE;
            } else if (!long_hint_beeped && now - pressed_at_us >= PWR_KEY_LONG_MIN_US) {
                /* 到达 3 秒长按窗：短鸣一声提示「可以松开」。 */
                long_hint_beeped = true;
                buzzer_beep(120);
                ESP_LOGI(TAG, "long press hint beep");
            }
            break;
        case PWR_IGNORE:
            if (!level_low) {
                state = PWR_IDLE;
            }
            break;
        default:
            state = PWR_IDLE;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PWR_KEY_POLL_MS));
    }
}

/** 拉高 SYS_EN 锁存系统供电。电池供电时按键松开后系统是否继续工作只取决于
 *  这一脚：不锁存（悬空或为低）时松手即断电，复位后没及时拉高同样如此，
 *  因此调用点必须是 app_main 的第一件事。USB 供电下锁存被旁路，拉高无副作用。 */
esp_err_t pwr_key_power_hold(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PWR_KEY_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power latch config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(PWR_KEY_HOLD_GPIO, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power latch hold failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "power latch held (SYS_EN GPIO%d high)", PWR_KEY_HOLD_GPIO);
    return ESP_OK;
}

esp_err_t pwr_key_start(pwr_key_fn callback, void *user)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PWR_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }
    s_callback = callback;
    s_user = user;
    if (xTaskCreate(pwr_key_task, "pwr-key", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

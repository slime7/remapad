#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化屏幕背光控制器 (LEDC PWM)
 */
esp_err_t backlight_init(void);

/**
 * 设置屏幕背光亮度 (0 - 100 百分比)
 */
esp_err_t backlight_set(uint8_t brightness_pct);

/**
 * 获取当前屏幕背光亮度
 */
uint8_t backlight_get(void);

#ifdef __cplusplus
}
#endif

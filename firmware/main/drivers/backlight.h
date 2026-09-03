#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 预留屏幕背光 PWM 驱动初始化接口。 */
esp_err_t backlight_init(void);

/** 设置预留的背光亮度接口（0 - 100）。 */
esp_err_t backlight_set(uint8_t brightness_pct);

/** 读取当前预留的背光亮度值。 */
uint8_t backlight_get(void);

#ifdef __cplusplus
}
#endif

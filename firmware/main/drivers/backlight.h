#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化背光 PWM 通道，初始为熄灭。面板可见性由调用方在合适时机
 * （如首帧提交成功后）调用 backlight_set 点亮。 */
esp_err_t backlight_init(void);

/** 设置背光亮度（0 - 100），超出范围自动收敛到 100。 */
esp_err_t backlight_set(uint8_t brightness_pct);

/** 读取当前背光亮度值。 */
uint8_t backlight_get(void);

#ifdef __cplusplus
}
#endif

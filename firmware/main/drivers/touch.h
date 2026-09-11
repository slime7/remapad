#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次采样得到的触点，坐标为逻辑视口像素（与面板方向一致）。 */
typedef struct {
    uint16_t x;
    uint16_t y;
} touch_contact_t;

/** 初始化 CST816T 触摸控制器：共享 I2C 总线（SCL=GPIO10, SDA=GPIO11）、
 * 复位与中断脚（GPIO13/GPIO14）。 */
esp_err_t touch_init(void);

/** 采样当前触点并写入 out，返回有效触点数（CST816T 为单点，最多 1）。
 * 未初始化或采样失败时返回 0，不影响下一次采样。 */
size_t touch_sample(touch_contact_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif

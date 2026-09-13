#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 板载蜂鸣器（GPIO42，无源，LEDC tone 输出）：PWR 长按到 3 秒时短鸣一声，
 * 提示用户可以松开。beep 非阻塞（输出启动后由 esp_timer 定时关闭），可在
 * 任意任务上下文调用。
 */

esp_err_t buzzer_init(void);

/** 短鸣一次，on_ms 为发声时长（毫秒，超范围按 120ms 处理）。 */
void buzzer_beep(uint32_t on_ms);

#ifdef __cplusplus
}
#endif

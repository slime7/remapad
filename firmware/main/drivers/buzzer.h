#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 板载蜂鸣器（GPIO42，无源，LEDC tone 输出）：完全关机后重新上电时短鸣一声
 * 作开机反馈，PWR 长按到 3 秒再短鸣一声提示用户可以松开。beep 非阻塞（输出
 * 启动后由 esp_timer 定时关闭），可在任意任务上下文调用。
 */

esp_err_t buzzer_init(void);

/** 短鸣一次，on_ms 为发声时长（毫秒，超范围按 120ms 处理）。 */
void buzzer_beep(uint32_t on_ms);

/** 按指定音高短鸣一次（freq_hz 0 或超范围按缺省 4kHz 处理）：无源蜂鸣器
 *  的音高随 LEDC 定时器走，触觉采样的「发声」段按音色表给音高（定位呼叫
 *  的两声上行短鸣），不再是固定刺耳的单频。 */
void buzzer_beep_tone(uint32_t freq_hz, uint32_t on_ms);

#ifdef __cplusplus
}
#endif

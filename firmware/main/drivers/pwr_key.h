#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PWR 按键（板卡电源功能电路：SYS_OUT=GPIO40 为按键电平，SYS_EN=GPIO41
 * 保持脚）。本驱动只采样 GPIO40，不触碰 SYS_EN：USB 供电下电源锁存被
 * 旁路，电池供电场景的保持时序待电源 BSP 阶段确认后再接入。
 *
 * 事件在 pwr-key 任务上下文回调（内部 RAM 栈，可安全调用背光/队列接口，
 * 但不得触碰 PocketJS guest）：
 * - 短按（<600ms 释放）：息屏 / 亮屏切换；
 * - 长按（3-6s 释放）：切换连接模式（USB 角色 device ↔ host）；
 * - 按住超过 6s 不产生软件事件（让位硬件电源行为），其余时长忽略。
 */

typedef enum {
    PWR_KEY_SHORT = 0,
    PWR_KEY_LONG = 1,
} pwr_key_event_t;

typedef void (*pwr_key_fn)(pwr_key_event_t event, void *user);

/** 启动按键采样任务（GPIO40 输入上拉，10ms 轮询去抖）。 */
esp_err_t pwr_key_start(pwr_key_fn callback, void *user);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PWR 按键与电源保持（板卡电源功能电路：SYS_OUT=GPIO40 为按键电平，
 * SYS_EN=GPIO41 为电源保持脚）。
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

/** 拉高 SYS_EN 锁存系统供电：电池供电时 PWR 键松开后靠它维持供电。
 *  属于上电时序，必须在 app_main 入口调用（早于外设与 UI 初始化）；
 *  软件关机走 pwr_key_power_release。 */
esp_err_t pwr_key_power_hold(void);

/** 拉低 SYS_EN 释放电源锁存：电池供电时系统随即断电；USB 供电下锁存被
 *  旁路、系统仍在运行，调用方需要重新锁存（见 pwr_key_power_hold）。 */
esp_err_t pwr_key_power_release(void);

/** 启动按键采样任务（GPIO40 输入上拉，10ms 轮询去抖）。 */
esp_err_t pwr_key_start(pwr_key_fn callback, void *user);

#ifdef __cplusplus
}
#endif

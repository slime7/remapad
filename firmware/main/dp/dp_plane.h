#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 控制器数据面（ADR 0011）：启动 BLE host 任务与数据面任务。
 * 数据面任务固定 5ms 周期执行 输入源采样 -> 规范化 -> NS2 编码 -> BLE 通知，
 * 高频路径不经过 PocketJS turn / JSON bridge。
 * 当前输入源为合成测试源（BLE 链路验证用），M5 由 USB host 输入替换。
 */

esp_err_t dp_plane_start(void);

/**
 * 调试注入：在数据面当前输入状态上叠加一次按键按下（约 250ms 后自动释放）。
 * 这是控制面进入数据面的唯一低频通道，供调试页手动验证 BLE 上报链路；
 * 高频采样与编码仍由数据面任务独立完成。
 */
void dp_plane_debug_key(uint32_t buttons_mask);

#ifdef __cplusplus
}
#endif

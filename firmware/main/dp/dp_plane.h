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

#ifdef __cplusplus
}
#endif

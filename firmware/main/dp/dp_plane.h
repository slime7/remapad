#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 控制器数据面（ADR 0011）：启动 BLE host 任务与数据面任务。
 * 数据面任务固定 5ms 周期执行 输入源采样 -> 规范化 -> NS2 编码 -> BLE 通知，
 * 高频路径不经过 PocketJS turn / JSON bridge。
 * 当前输入源为合成测试源，静置无按键（BLE 链路验证用），按键输入仅来自
 * 下方的调试注入；M5 由 USB host 输入替换。
 */

esp_err_t dp_plane_start(void);

/**
 * 调试注入：在数据面当前输入状态上叠加一次按键按下，保持 hold_ms 后自动
 * 释放。这是控制面进入数据面的唯一低频通道，供调试页手动验证 BLE 上报
 * 链路（主机 Grip / 顺序页此时能看到按键变化）；高频采样与编码仍由数据面任务
 * 独立完成。
 */
void dp_plane_debug_key(uint32_t buttons_mask, uint32_t hold_ms);

/**
 * 手动反馈注入（串口联调用）：把一次反馈事件叠加进持续帧（fields 用
 * pad/feedback.h 的 PAD_FEEDBACK_FIELD_* 位），数据面任务下一拍按接入设备的
 * 布局编码并投递——与主机反馈走完全同一条路径。可用于没有主机在场时，
 * 从串口脚本验证震动 / 玩家灯 / 触觉采样到实体手柄的整条反馈链路。
 */
void dp_plane_inject_feedback(uint8_t fields, const pad_feedback_t *event);

/** 读出当前持续反馈帧（串口回读用）：叠加后的震动、玩家灯与触觉采样。 */
void dp_plane_feedback_held(pad_feedback_t *out);

#ifdef __cplusplus
}
#endif

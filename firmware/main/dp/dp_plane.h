#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 控制器数据面：启动 BLE host 任务与数据面任务。数据面固定 5ms 周期做
 * 输入源采样 -> 规范化 -> NS2 编码 -> BLE 通知，高频路径不经过界面每帧轮询与 JSON bridge。
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
 * 手动反馈注入（串口联调用）：事件叠加进持续帧（fields 用 pad/feedback.h 的位），
 * 数据面下一拍按接入设备的布局编码投递，与主机反馈走同一条路径。
 */
void dp_plane_inject_feedback(uint8_t fields, const pad_feedback_t *event);

/** 读出当前持续反馈帧（串口回读用）：叠加后的震动、玩家灯与触觉采样。 */
void dp_plane_feedback_held(pad_feedback_t *out);

/**
 * 桥接路径的音频触觉让位开关（PC 经 CLI `haptic audio on|off` 告知）：开时
 * 数据面给桥接发的输出报告把震动字段清零——触觉由 PC 侧在 DS5 的音频端点上
 * 合成，同一对音圈被 HID 与音频双驱动会叠成浑浊触感。桥接断开
 * （input_source_attached 为假）时自动失效，PC 下次接入按最新告知生效。
 */
void dp_plane_bridge_audio_haptics(bool on);

/** 让位开关的当前值（cli_feedback_state 回显用）。 */
bool dp_plane_bridge_audio_active(void);

#ifdef __cplusplus
}
#endif

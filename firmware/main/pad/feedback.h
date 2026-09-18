#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 输出报告缓冲上限：最长的是 DualSense 蓝牙形态（78 字节）。 */
#define PAD_OUTPUT_MAX 78

/**
 * 反馈方向的编码入口（处理段）：把主机下发的反馈（pad_feedback_t）按输入
 * 设备的布局行编码成该设备能吃的输出报告，首字节是 Report ID，交给传输侧
 * （USB host 直插写 OUT 端点，桥接回传给 PC 写手柄）原样写出。
 *
 * 同代透传：NS2 手柄原样接收主机的 LRA 参数包（USB 形态 Report 0x02），
 * 因此不需要任何字段映射；其余家族按布局行的 out 描述写震动强度、玩家灯
 * 与退化的触觉采样。返回编码长度，没有可写的反馈通道时返回 0。
 */
size_t pad_feedback_encode(pad_conn_t conn, uint16_t vid, uint16_t pid,
                           const pad_feedback_t *feedback, uint8_t *out, size_t out_len);

/** 上次编码命中的布局行（诊断；未命中或无反馈通道时为 NULL）。 */
const pad_layout_t *pad_feedback_last_layout(void);

/** 反馈事件带来的字段（pad_feedback_apply 的 fields 位）。 */
typedef enum {
    PAD_FEEDBACK_FIELD_RUMBLE = 1u << 0,
    PAD_FEEDBACK_FIELD_PLAYER_LED = 1u << 1,
    PAD_FEEDBACK_FIELD_HAPTIC = 1u << 2,
} pad_feedback_field_t;

/**
 * 把一次主机反馈事件叠加到持续帧上：事件带哪些字段就覆盖哪些字段，其余沿用
 * 上一帧。震动与玩家灯是主机的持续状态——事件之间回落到默认值会把刚点亮的
 * 玩家灯写灭、马达强度来回跳；触觉采样是事件式的，只在带它的事件里更新
 * （0x00 是「停止播放」），非采样事件不清——载波包以接近输入上报的频率
 * 到达，顺手清会把脉冲切碎（查找手柄页的蜂鸣时有时无），收尾兜底是数据面
 * 的超时自灭。
 */
void pad_feedback_apply(pad_feedback_t *held, uint8_t fields, const pad_feedback_t *event);

/**
 * 两帧反馈的写回语义是否等价：主机的震动流是音频式连续包络，以接近输入
 * 上报的频率到达，原始参数包却逐包都在抖（低有效位、频率扫描），所以判定
 * 只跟会改变马达/灯字段的语义值走（使能、两带强度、玩家灯、非零触觉采样），
 * 原始 LRA 参数包不参与。投递侧据此做「值变化才发」，避免把串口/传输通道
 * 灌满；同代透传的原始包另由编码字节的变化判据把关（见 dp 的写回去重）。
 */
bool pad_feedback_equal(const pad_feedback_t *a, const pad_feedback_t *b);

#ifdef __cplusplus
}
#endif

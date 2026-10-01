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

/** 采样音色里强震段的幅度（0-255，大于 0 即发声——蜂鸣器不调音量，
 *  幅度只区分「响」与「停顿」两态）。 */
#define PAD_HAPTIC_PULSE 0xC0u
/** 采样音色里蜂鸣段的幅度：与强震同响，只在时间轴上形成
 *  「震动、停顿、发声、停顿」的节奏。 */
#define PAD_HAPTIC_BEEP 0x80u

/** 反馈状态线格式的两代长度：16 字节 = 基础段（使能、两带强度、玩家灯、
 *  采样与两带频率），57 字节 = 追加 HD 时序子帧表（PC 侧哑渲染的输入）。 */
#define PAD_FEEDBACK_WIRE_LEGACY 16u
#define PAD_FEEDBACK_WIRE_HD 57u

/** 编码手柄输出报告。返回报告长度，无通道返回 0。 */
size_t pad_feedback_encode(pad_conn_t conn, uint16_t vid, uint16_t pid, const pad_feedback_t *feedback, uint8_t *out,
                           size_t out_len);

/** 上次编码命中的布局行（诊断；未命中或无反馈通道时为 NULL）。 */
const pad_layout_t *pad_feedback_last_layout(void);

/** 将主机反馈波形转换为手柄布局的 HD 触觉序列。无 HD 支持时输出全零。 */
void pad_feedback_hd_render(const pad_layout_t *layout, const pad_feedback_t *feedback, pad_hd_render_t *out);

/** 序列化反馈状态为线格式（16 字节基础段或 57 字节 HD 扩展段）。返回实际写入字节数。 */
size_t pad_feedback_wire(const pad_feedback_t *feedback, const pad_hd_render_t *hd, uint8_t *out, size_t cap);

/** 反馈事件带来的字段（pad_feedback_apply 的 fields 位）。 */
typedef enum {
  PAD_FEEDBACK_FIELD_RUMBLE = 1u << 0,
  PAD_FEEDBACK_FIELD_PLAYER_LED = 1u << 1,
  PAD_FEEDBACK_FIELD_HAPTIC = 1u << 2,
} pad_feedback_field_t;

/** 将主机反馈事件叠加到持续状态帧中（按 fields 掩码增量覆盖）。 */
void pad_feedback_apply(pad_feedback_t *held, uint8_t fields, const pad_feedback_t *event);

/** 比较两帧反馈的马达、LED 与触觉语义是否等价。 */
bool pad_feedback_equal(const pad_feedback_t *a, const pad_feedback_t *b);

/** 检查采样音色的段状态与段音高是否发生改变。 */
bool pad_feedback_segment_changed(const pad_feedback_t *sent, uint8_t env, uint16_t tone_hz);

/** 编码音频触觉接手音圈时的让位输出报告（马达清零并应用 quiet_presets）。 */
size_t pad_feedback_encode_quiet(pad_conn_t conn, uint16_t vid, uint16_t pid, const pad_feedback_t *feedback,
                                 uint8_t *out, size_t out_len);

/** 主机线性震动振幅至 ERM 偏心马达的感知非线性重映射。 */
uint8_t pad_rumble_perceived(uint8_t amp);

/** 重置 DualSense 蓝牙输出报告序号计数器。 */
void pad_feedback_bt_seq_reset(void);

/** 查询采样音色在指定毫秒偏移时的当前段幅度、剩余毫秒数及音高。 */
uint8_t pad_haptic_pulse_step(uint8_t sample, uint32_t age_ms, uint32_t *remain_ms, uint16_t *tone_hz);

/** 采样音色当前时刻的渲染幅度（pad_haptic_pulse_step 的只取幅度形态）。 */
uint8_t pad_haptic_pulse_envelope(uint8_t sample, uint32_t age_ms);

/** 将采样音色幅度叠加合并至左右马达震动值。 */
void pad_feedback_fold_pulse_motors(pad_feedback_t *feedback, uint8_t amp);

#ifdef __cplusplus
}
#endif

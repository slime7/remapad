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

/**
 * HD 触觉映射（映射在布局内完成）：按布局行的 hd 规则把主机波形
 * （rumble_keys 的时序子帧）重整成该设备的子帧序列——子帧按时间顺序各播
 * cycle_ms/3，低频给冲击、高频给纹理，振幅线性直迁（音圈没有 ERM 死区，
 * 不做感知重映射），频率按 hd 的范围夹取、0 回落缺省；采样音色的「强震」
 * 段以 hd.pulse_hz 覆盖各子帧的音圈、「发声」段以 hd.beep_hz 铺到扬声器。
 * 布局行没有 HD 通路时输出全零。
 */
void pad_feedback_hd_render(const pad_layout_t *layout, const pad_feedback_t *feedback,
                            pad_hd_render_t *out);

/**
 * 反馈状态线格式（桥接 FEEDBACK 帧载荷）：前 16 字节是基础段（使能、两带
 * 强度、玩家灯、采样与两带频率，老 PC 按长度识别），hd 非 NULL 且布局声明
 * 了 HD 通路时追加到 57 字节（每侧时序子帧表 + 扬声器音色，PC 侧音频触觉/
 * 蓝牙私有流的哑渲染输入）。返回实际长度，cap 不够且需要 HD 段时返回 0。
 */
size_t pad_feedback_wire(const pad_feedback_t *feedback, const pad_hd_render_t *hd,
                         uint8_t *out, size_t cap);

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

/**
 * 采样音色的段状态（幅度段 + 段音高）是否与已投递的不同：段由固件按采样 ID
 * 的时序合成（主机只给起停与 ID），而投递原先只在主机事件到达时发生——查找
 * 手柄页的采样事件约 15Hz，段边界因此被量化到 64ms 的栅格上（震动/蜂鸣的
 * 起止错位、短段整段丢失）。数据面每 tick 用本判据置待发位，投递精度回到
 * tick（5ms）。tone_hz 只在「发声」段参与（其他段不铺扬声器）。
 */
bool pad_feedback_segment_changed(const pad_feedback_t *sent, uint8_t env, uint16_t tone_hz);

/**
 * 让位版本的输出报告：音频触觉接手音圈时用的编码形态——马达字节恒零、
 * `valid_flag0` 换成布局行 `quiet_presets` 声明的那一组（DS5 两行是 0xA1：
 * 音频控制与音量档照旧，震动位段是「带 COMPATIBLE_VIBRATION、不带
 * HAPTICS_SELECT」的那次交还切换）。手柄的音圈模式是粘性的：HAPTICS_SELECT
 * 置位后停在震动仿真模式、之后的触觉 PCM 被静音，因此让位期间不能照抄完整
 * 预置的 0xA3，也不能把 `valid_flag0` 整个留零（留零等于不交还，音圈停在
 * 震动仿真模式、马达字节又已清零，表现是「没有震动、只剩玩家灯」）。
 * 布局行没声明 `quiet_presets` 时与 pad_feedback_encode 同形。
 */
size_t pad_feedback_encode_quiet(pad_conn_t conn, uint16_t vid, uint16_t pid,
                                 const pad_feedback_t *feedback, uint8_t *out,
                                 size_t out_len);

/**
 * 主机震动振幅的感知重映射（0-255 → 0-255）：NS2 的振幅是 LRA 线性驱动档位，
 * 小档位在共振频点上也能摸到；ERM 偏心马达（DualSense / DualShock / Xbox）
 * 低占空比整段落在死区里，线性直迁会让游戏里中低强度的震动几乎无感
 * （USB 直插游戏震动非常轻）。按 out = 40 + 215·√(amp/255)
 * （amp > 0）抬低端、压顶端，0 仍映射 0。采样音色与 CLI 注入写的是设备刻度，
 * 不经过本表。
 */
uint8_t pad_rumble_perceived(uint8_t amp);

/** 蓝牙序号回零（测试与诊断入口）：下一条 DualSense 蓝牙输出报告的序号
 *  半字节从 0 重新开始，黄金 CRC 用例据此复算。 */
void pad_feedback_bt_seq_reset(void);

/**
 * 采样音色在 age_ms 的当前段：返回段内幅度（0 = 停顿），remain_ms（可空）
 * 给出距下一段段边界的毫秒数，tone_hz（可空）给出「发声」段的音高（Hz，
 * 0 = 该段不发声或用布局行的 beep_hz 缺省）。主机只发采样 ID、不带播放形态
 * （同一 ID 以十几 Hz 重发），真手柄的节奏由其内部音色库给
 * 出——本设备对应的就是这里的采样音色表：按 ID 登记各自的响/停时间线
 * （段边界毫秒 → 段内幅度与音高），未登记的采样回落缺省音色（一次短脉冲
 * 后静默）。登记条目按各自周期循环播放，缺省音色不循环（主机要重复播放就
 * 用 0x00 收掉再发）。age_ms 是自采样起播（effective 值从无到有）起的
 * 毫秒数；数据面据此驱动板载蜂鸣器按段发声（USB 直插），蓝牙桥接路径的
 * 发声段由 HD 通路折进音圈。
 */
uint8_t pad_haptic_pulse_step(uint8_t sample, uint32_t age_ms, uint32_t *remain_ms,
                              uint16_t *tone_hz);

/** 采样音色当前时刻的渲染幅度（pad_haptic_pulse_step 的只取幅度形态）。 */
uint8_t pad_haptic_pulse_envelope(uint8_t sample, uint32_t age_ms);

/**
 * 采样「强震」段折进两侧马达（本地写回兜底用）：该条写回通路没有音频触觉
 * 承载（蓝牙没开 0x32/0x36 私有流、音频端点开不起来）时，查找手柄的强震段
 * 只剩马达这一条出路——把段幅度并进两侧马达（取 max 不降档，正在震的一侧
 * 沿用更强的值），amp 为 0（发声/停顿段）不动马达。只改传入的帧，不动
 * 持续帧与 FEEDBACK 状态帧：PC 侧合成看到的仍是主机的真实波形。
 */
void pad_feedback_fold_pulse_motors(pad_feedback_t *feedback, uint8_t amp);

#ifdef __cplusplus
}
#endif

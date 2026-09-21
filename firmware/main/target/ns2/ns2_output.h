#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "pad_state.h"
#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 输出封装：数据面把规范化手柄状态发往主机，不需要关心报告格式、计数器与传输通道。
 * 输入侧按会话当前报告格式编码并经注册的输出通道发送；反馈侧把主机事件分发给监听者；
 * 电池与 NFC 状态字节由本模块汇总进报告。协议见 docs/controller-switch2.md。
 */

/** 输出通道：把编码后的报告体发往 NS2 主机链路（BLE 现役，USB 预留）。
 * 通道可承载多个并发的输出会话（一条 BLE 连接一个），ns2_output_send 逐
 * 会话按报告格式编码发送。 */
typedef struct {
    /** 当前活跃会话数。 */
    size_t (*session_count)(void *user);
    /** 第 index 个会话的身份（ns2_identity_t）与报告格式（0x05 / 0x09）。 */
    bool (*session_info)(size_t index, uint8_t *identity, uint8_t *report_format, void *user);
    /** 向第 index 个会话发送编码好的报告体（连接未订阅时由通道内部丢弃）。 */
    void (*send_report)(size_t index, uint8_t report_id, const uint8_t *body, size_t len,
                        void *user);
    void *user;
} ns2_output_sink_t;

/** 主机反馈事件类型（结构化，转发方按类型取对应 payload）。 */
typedef enum {
    NS2_FEEDBACK_RUMBLE = 0,   /* payload = ns2_rumble_event_t */
    NS2_FEEDBACK_PLAYER_LED,   /* payload = uint8_t 掩码 bit0-3 */
    NS2_FEEDBACK_HAPTIC_SAMPLE, /* payload = uint8_t 采样 ID */
} ns2_feedback_type_t;

/** Output Report 0x02 的结构化解析结果：左右 LRA 使能与原始参数包。 */
typedef struct {
    bool left_on;
    bool right_on;
    /** 原始 2×16B LRA 参数包（状态字 + 3 组音调/振幅指令），供转发方
     *  按目标设备能力二次编码。 */
    uint8_t raw[32];
} ns2_rumble_event_t;

/**
 * LRA 参数包里一个时序子帧的解码结果：每个子帧一条低频音与一条高频音，
 * 频率已按 9 位 log2 刻度解成 Hz、振幅是 LRA 原始档位，夹取与回落由消费侧的布局映射决定。
 * 位布局见 docs/controller-switch2.md 的输出报告一节。
 */
typedef pad_rumble_key_t ns2_rumble_key_t;

typedef void (*ns2_feedback_fn)(ns2_feedback_type_t type, const void *payload, void *user);

/** 解析主机经 0x0012 下发的 Output Report 0x02 为结构化震动事件：左右 LRA 各 16 字节参数包，
 *  状态字 bit6 是启用标志；载荷按长度自适应（≥33 字节剥离前缀）。命中返回 true，过短返回 false。 */
bool ns2_rumble_parse(const uint8_t *data, size_t len, ns2_rumble_event_t *out);

/**
 * 从 16 字节 LRA 参数包按频带取振幅：三个时序子帧各带低频（10 位）与高频
 * （8 位）振幅，逐带取最大值、压到 8 位刻度（0-255）。主机的震动流是连续
 * 包络（低频给冲击、高频给纹理），映射设备的马达前先按带拆开。
 */
void ns2_rumble_band_strengths(const uint8_t raw[16], uint8_t *lf, uint8_t *hf);

/**
 * 从 16 字节 LRA 参数包按频带取驱动频率：三个时序子帧各带低频与高频 9 位
 * log2 频率码（位 20-28 / 位 0-8），解成 Hz（f = 10×2^(码/128)）后逐带取
 * 最大。码 0 按「未声明」报 0，消费侧（音频触觉合成）自行夹取与回落。
 */
void ns2_rumble_band_frequencies(const uint8_t raw[16], uint16_t *lf_hz, uint16_t *hf_hz);

/**
 * 从 16 字节 LRA 参数包解出全部 3 个时序子帧（NS2 波形规则的完整形态）：
 * 每个子帧的高/低频频率（落地 Hz）与振幅（10 位刻度）原样给出，不做归一。
 * 返回有效子帧数（状态字的操作数计数，0 按 3 处理）。HD 触觉映射按它把
 * 主机的波形按时间顺序重整为目标设备的 PCM，而不是只留两带最大值。
 */
size_t ns2_rumble_keys(const uint8_t raw[16], ns2_rumble_key_t keys[PAD_RUMBLE_KEY_COUNT]);

/**
 * 从 16 字节 LRA 参数包估一个 0-255 强度：两带振幅取大，供「在震」判定与
 * 不分带的设备使用（= ns2_rumble_band_strengths 结果的较大者）。
 */
uint8_t ns2_rumble_strength(const uint8_t raw[16]);

/** 注册输出通道（BLE 通知在 ble_controller 就绪后由 dp 注册）。重复注册
 *  覆盖旧通道。 */
void ns2_output_set_sink(const ns2_output_sink_t *sink);

/** 订阅主机反馈事件（单监听者；USB 转发在此注册）。 */
void ns2_output_set_feedback_listener(ns2_feedback_fn fn, void *user);

/** 以规范化状态发送一个输入报告周期：按键 / 摇杆 / 电池 / NFC 状态一并
 *  编码进当前会话格式。内部维护两种格式的循环计数器。 */
void ns2_output_send(const ns2_controller_state_t *state);

/** 同代透传（NS2 手柄接在板卡上）：把设备原始报文体原样发给身份与报告格式都对得上的会话，
 *  只重写由本机会话决定的状态字节；没有匹配会话时返回 false，调用方走解析重编码。 */
bool ns2_output_send_raw(const pad_state_t *pad);

/** 会话侧事实：主机是否开启了触觉特性（0x09 状态标志字节与透传重写都用它）。 */
void ns2_output_set_rumble_enabled(bool enabled);

/** 更新随报告上发的电池信息（电平 0-9、电压毫伏、充电与外部供电）。 */
void ns2_output_set_battery(uint8_t level, uint16_t voltage_mv, bool charging, bool external);

/** 0x09 运动块占位方式（ns2_motion_mode_t）：板卡无 IMU，用 CLI `motion`
 *  在几种占位间切换，无需重新烧录。 */
void ns2_output_set_motion_mode(uint8_t mode);

/** 当前运动块占位方式（CLI 回显用）。 */
uint8_t ns2_output_motion_mode(void);

/**
 * 耳机状态字节（0x09 偏移 0x0D，NS2_HEADSET_*）：生效值优先取覆盖值，
 * 否则取输入设备派生的 auto 值；编码路径与同代透传路径都从这里取，
 * 保证两边的 0x09 与 0x05 对得上。
 */
uint8_t ns2_output_headset_byte(void);

/** auto 模式下的派生取值（由目标侧按输入设备的 3.5mm 状态更新）。 */
uint8_t ns2_output_headset_derived(void);
void ns2_output_set_headset_derived(uint8_t value);

/** 覆盖开关与取值（串口 headset 命令）：enabled 为 true 时钉住一个值做
 *  对照，false 回到 auto；out_value 非 NULL 时回读当前覆盖值。 */
bool ns2_output_headset_override(uint8_t *out_value);
void ns2_output_set_headset_override(bool enabled, uint8_t value);

/** 输入报告 NFC 状态字节：透传与重编码两条路径共用的取值源（ns2_nfc 的
 *  标签模拟状态机：开轮询且预置镜像 0x01，否则 0x00）。 */
uint8_t ns2_output_nfc_state(void);

/* --- 以下由 ble_session 在解析主机输出后调用（结构化反馈入口）--- */

void ns2_output_emit_rumble(const ns2_rumble_event_t *event);
void ns2_output_emit_player_led(uint8_t led_mask);
void ns2_output_emit_haptic_sample(uint8_t sample_id);

/**
 * 从命令帧解出触觉采样 ID（Command 0x0A）：命中返回 true 并把采样 ID 写进
 * sample（0x00 = 停止播放）。纯函数，供 ble_session 的命令分发与主机端
 * 样本回放共用。
 */
bool ns2_haptic_sample_parse(const uint8_t *frame, size_t len, uint8_t *sample);

#ifdef __cplusplus
}
#endif

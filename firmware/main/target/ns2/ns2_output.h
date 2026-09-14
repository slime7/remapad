#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 输出封装：数据面（以及将来的 USB/桥接路径）调用本模块把规范化手柄
 * 状态发往 NS2 主机，不需要关心报告格式、计数器与传输通道。
 *
 * - 输入侧：ns2_output_send() 接收一个规范化状态（可只填需要输出的按键，
 *   其余字段走默认），内部按会话当前的报告格式（0x05 / 0x09）编码并经
 *   注册的输出通道发送；默认通道为 BLE 通知，USB 通道接入后注册替换。
 * - 反馈侧：主机下发的震动 / 玩家 LED / 触觉采样被 ble_session 解析成
 *   结构化事件后经 ns2_output_emit_* 分发给监听者（当前由 dp 记录日志，
 *   M5 起转发给插入的 USB 手柄或桥接 PC），返回信息结构化、便于解析。
 * - 电池 / amiibo：ns2_output_set_battery 更新随报告上发的电源字段；
 *   amiibo 镜像先经 ns2_output_amiibo_stage 预置在设备内存（传输方式
 *   待定：bridge 分块 / storage 文件 / USB 均可），NFC 命令通路接通后
 *   由会话层经 ns2_output_amiibo_read 分块取用。
 */

/** 输出通道：把编码后的报告体发往 NS2 主机链路（BLE 现役，USB 预留）。
 * 通道可承载多个并发的输出会话（BLE 连接；JoyCon 组合为左右两条），
 * ns2_output_send 按会话身份切分状态后逐会话编码发送。 */
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

typedef void (*ns2_feedback_fn)(ns2_feedback_type_t type, const void *payload, void *user);

/** 解析主机经 0x0012 下发的 Output Report 0x02（BLE 形态）为结构化震动事件：
 *  左/右 LRA 各 16 字节参数包，状态字 bit6 为启用标志。实机写入的载荷是
 *  32 字节（两个参数包，BLE 模式不带 Report ID），部分主机路径会多带 1 字节
 *  Report ID/占位前缀（33 字节）。命中返回 true，过短返回 false。 */
bool ns2_rumble_parse(const uint8_t *data, size_t len, ns2_rumble_event_t *out);

/** 注册输出通道（BLE 通知在 ble_controller 就绪后由 dp 注册）。重复注册
 *  覆盖旧通道。 */
void ns2_output_set_sink(const ns2_output_sink_t *sink);

/** 订阅主机反馈事件（单监听者；M5 的 USB 转发注册于此）。 */
void ns2_output_set_feedback_listener(ns2_feedback_fn fn, void *user);

/** 以规范化状态发送一个输入报告周期：按键 / 摇杆 / 电池 / NFC 状态一并
 *  编码进当前会话格式。内部维护两种格式的循环计数器。 */
void ns2_output_send(const ns2_controller_state_t *state);

/** 更新随报告上发的电池信息（电平 0-9、电压毫伏、充电与外部供电）。 */
void ns2_output_set_battery(uint8_t level, uint16_t voltage_mv, bool charging, bool external);

/** 0x09 运动块占位方式（ns2_motion_mode_t）：板卡无 IMU，实机排查「主机不
 *  采用输入」时用 CLI `motion` 切换，无需重新烧录。 */
void ns2_output_set_motion_mode(uint8_t mode);

/** 当前运动块占位方式（CLI 回显用）。 */
uint8_t ns2_output_motion_mode(void);

/** 预置 amiibo / NTAG215 镜像（最长 NS2_AMIIBO_MAX 字节，拷贝进 PSRAM）。
 *  成功后 ns2_output_nfc_state() 汇报 0x01（已就绪待感应），供输入报告
 *  的 NFC 状态字节使用；传 NULL/0 清除。 */
esp_err_t ns2_output_amiibo_stage(const uint8_t *data, size_t len);

/** 当前预置的 amiibo 镜像是否就绪。 */
bool ns2_output_amiibo_ready(void);

/** 会话层 NFC 命令通路读取预置镜像（偏移越界部分补 0xFF），返回实际长度。 */
size_t ns2_output_amiibo_read(uint32_t offset, uint8_t *out, size_t len);

/** 输入报告 NFC 状态字节：0x00 空闲；预置就绪 0x01（待主机轮询语义由
 *  NFC 命令通路实现后扩展为感应流程状态）。 */
uint8_t ns2_output_nfc_state(void);

/* --- 以下由 ble_session 在解析主机输出后调用（结构化反馈入口）--- */

void ns2_output_emit_rumble(const ns2_rumble_event_t *event);
void ns2_output_emit_player_led(uint8_t led_mask);
void ns2_output_emit_haptic_sample(uint8_t sample_id);

#ifdef __cplusplus
}
#endif

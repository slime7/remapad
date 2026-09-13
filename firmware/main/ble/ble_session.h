#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 手柄会话：广播策略、连接初始化时序与指令分发（controller.md §6/§10.2）。
 * 传输细节（NimBLE、GATT 表、notify）由 ble_controller 承载，本模块只面对协议。
 * 会话按连接分槽（最多 2 条）：Pro 单会话；JoyCon 组合左右两只各一个会话，
 * 各自独立的身份、报告格式、配对状态与凭证（凭证按身份分槽持久化）。
 */

/** host 同步完成（栈就绪）：记录自身 MAC 并启动标准发现广播。 */
void ns2_session_on_sync(const uint8_t own_mac[6]);

/** ACL 连接建立（identity 为该连接呈现的手柄身份）。 */
void ns2_session_on_connect(uint16_t conn_handle, uint8_t identity);

/** 连接建立失败：恢复广播，不影响会话状态。 */
void ns2_session_on_connect_fail(void);

/** 记录一次主机协议活动（ATT 读写/订阅），刷新所在连接的空闲计时。 */
void ns2_session_touch(uint16_t conn_handle);

/** 指定连接是否已超时无活动：连接中、握手未完成且超过空闲时限。
 * 供周期检查断开手机/PC 等只连不聊的回连方；主机初始化毫秒级到达，不受影响。 */
bool ns2_session_conn_idle_expired(uint16_t conn_handle);

/** 周期任务（1s，ble_controller 空闲定时器驱动）：固件假升级会话超时收尾。 */
void ns2_session_tick(void);

/** 断连：复位该连接的会话并按身份恢复广播。 */
void ns2_session_on_disconnect(uint16_t conn_handle, uint8_t identity);

/** Command 通道（0x0014）写入：8 字节帧头 + 应答体，BLE 传输层。 */
void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport,
                            uint16_t conn_handle);

/** 震动通道（0x0012）写入：Output Report 0x02。本阶段解析记录，M5 转发 USB。 */
void ns2_session_on_output(const uint8_t *data, size_t len, uint16_t conn_handle);

/** 复合输出通道（0x0016）写入：震动参数 + 指令帧。 */
void ns2_session_on_composite(const uint8_t *data, size_t len, uint16_t conn_handle);

/** 固件升级数据块（0x0018 WRITE NO RSP）：假升级会话入口——接收计数、
 * 静默超时后递增上报版本并落盘（假装升级到新版本）。 */
void ns2_session_on_fw_upgrade(const uint8_t *data, size_t len);

/** 特性掩码 bit5（触觉震动）是否在任一活跃会话开启，影响 0x09 状态标志字节。 */
bool ns2_session_rumble_enabled(void);

/** UI 手动配对：切换到标准发现广播（Pro 单身份 / JoyCon 双身份）。 */
void ns2_session_start_pairing_mode(void);

/** 结束手动配对：按凭证状态恢复回连或发现广播。 */
void ns2_session_stop_pairing_mode(void);

/** 手动配对模式是否开启（供控制面推导 UI 六态）。 */
bool ns2_session_pairing_mode_active(void);

/** 当前模式下配对是否完成：Pro 看单身份凭证；JoyCon 组合要求左右都配对。 */
bool ns2_session_paired(void);

/** 任一活跃连接的主机是否已注册：凭证匹配回连，或本会话内完成 0x15 握手。 */
bool ns2_session_host_registered(void);

/** 是否存在已通过白名单、进入握手等待的连接（配对进行中证据）。 */
bool ns2_session_waiting_pair(void);

/** 解除配对：清除当前模式全部身份的 NVS 凭证并切回发现广播；下次配对需
 * 重走 0x15。仅供控制面显式触发，「停止配对」不经过本函数。 */
void ns2_session_unpair(void);

/** 配对页「按下 LR」（JoyCon 组合）：确保双身份发现广播在发，并向数据面
 * 注入 L+R 按键（主机 Grip 界面的组合确认动作）。 */
void ns2_session_press_lr(void);

/** 下发手柄身份（Pro 或 JoyCon 组合 + 配色）：重建出厂块序列号 / PID /
 * 配色与广播拓扑；host 已同步时立即生效。 */
void ns2_session_set_identity(bool joycon, uint32_t body_rgb,
                              uint32_t button_rgb, uint32_t grip_rgb);

/* --- 输出会话视图（供 dp 的输出通道接线；ns2_output 经 sink 间接调用）--- */

/** 活跃输出会话数（当前连接数）。 */
size_t ns2_session_output_count(void);

/** 第 index 个活跃会话的身份与报告格式。 */
bool ns2_session_output_info(size_t index, uint8_t *identity, uint8_t *report_format);

/** 向第 index 个活跃会话发送编码好的报告体（内部按其连接与订阅状态投递）。 */
void ns2_session_deliver_report(size_t index, uint8_t report_id, const uint8_t *body);

#ifdef __cplusplus
}
#endif

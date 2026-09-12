#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 手柄会话：广播策略、连接初始化时序与指令分发（controller.md §6/§10.2）。
 * 传输细节（NimBLE、GATT 表、notify）由 ble_controller 承载，本模块只面对协议。
 * 配对 Command 0x15 与凭证持久化在 M3 接入；高频输入路径不经过本模块。
 */

/** host 同步完成（栈就绪）：记录自身 MAC 并启动标准发现广播。 */
void ns2_session_on_sync(const uint8_t own_mac[6]);

/** ACL 连接建立。 */
void ns2_session_on_connect(uint16_t conn_handle);

/** 记录一次主机协议活动（ATT 读写/订阅），刷新连接空闲计时。 */
void ns2_session_touch(void);

/** 当前连接是否已超时无活动：连接中、握手未完成且超过空闲时限。
 * 供周期检查断开手机/PC 等只连不聊的回连方；主机初始化毫秒级到达，不受影响。 */
bool ns2_session_host_idle_expired(void);

/** 断连：复位会话并恢复发现广播。 */
void ns2_session_on_disconnect(void);

/** Command 通道（0x0014）写入：8 字节帧头 + 应答体，BLE 传输层。 */
void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport);

/** 震动通道（0x0012）写入：Output Report 0x02。本阶段解析记录，M5 转发 USB。 */
void ns2_session_on_output(const uint8_t *data, size_t len);

/** 复合输出通道（0x0016）写入：震动参数 + 指令帧。 */
void ns2_session_on_composite(const uint8_t *data, size_t len);

/** 当前输入报告格式（0x05 / 0x09，由 0x03/0x0A 选择，默认 0x09）。 */
uint8_t ns2_session_report_format(void);

/** 特性掩码 bit5（触觉震动）是否开启，影响 0x09 状态标志字节。 */
bool ns2_session_rumble_enabled(void);

/** UI 手动配对：切换到标准发现广播（目标 MAC 全零）。 */
void ns2_session_start_pairing_mode(void);

/** 结束手动配对：按凭证状态恢复回连或发现广播。 */
void ns2_session_stop_pairing_mode(void);

/** 手动配对模式是否开启（供控制面推导 UI 六态）。 */
bool ns2_session_pairing_mode_active(void);

/** 是否存在已配对主机凭证。 */
bool ns2_session_paired(void);

/** 当前连接中的主机是否已注册：凭证匹配回连，或本会话内完成 0x15 握手。
 * 属于协议层的配对成功证据，与 NVS 存储状态相互独立。 */
bool ns2_session_host_registered(void);

/** 当前连接是否为已通过 Nintendo 白名单、进入握手等待的主机。
 * 被白名单立即断开的连接（手机/PC 回连）不算，避免控制面状态闪烁。 */
bool ns2_session_waiting_pair(void);

/** 解除配对：清除 NVS 凭证并切回标准发现广播；下次配对需重走 0x15。
 * 仅供控制面显式触发，「停止配对」不经过本函数。 */
void ns2_session_unpair(void);

#ifdef __cplusplus
}
#endif

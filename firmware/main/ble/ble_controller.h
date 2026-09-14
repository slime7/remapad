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
 * NimBLE 手柄外设传输层（ADR 0010）：NimBLE 生命周期、GATT 表、原始广播与通知发送。
 * 协议语义（广播内容、指令应答、会话状态）由 ble_session 决策，本模块只负责收发。
 * GATT 表按 controller.md §4 的句柄布局注册（含占位描述符对齐，见源内注释）。
 *
 * 连接最多并发 2 条：JoyCon 组合模式下左右两只同时在线（各自广播实例、
 * 身份与通知状态）；Pro 模式单连接。广播实例 0/1 可分别携带独立地址
 * （静态随机，JoyCon 双身份）或共用公共伪装地址（Pro 单身份双 PDU）。
 */

/** 初始化 NimBLE 并启动 host 任务；成功后栈在同步回调里触发发现广播。 */
esp_err_t ble_controller_start(void);

/** 是否处于 ACL 连接中（任一连接）。 */
bool ble_controller_connected(void);

/** 当前并发连接数（0-2）。 */
size_t ble_controller_conn_count(void);

/** 主动断开的 HCI 原因码（NimBLE hci_err 取值子集）。
 * 0x3E 连接建立失败会让手机等回连方按失败退避冷却，而非立刻重试。 */
#define BLE_CTL_DISCONNECT_USER_TERM 0x13
#define BLE_CTL_DISCONNECT_CONN_FAIL 0x3E

/** 主动断开全部连接并携带 HCI 断开原因。 */
void ble_controller_disconnect(uint8_t hci_reason);

/** 读取对端主机蓝牙地址（NimBLE 存储序，即显示序反转，与配对线格式一致）。 */
bool ble_controller_peer_mac(uint16_t conn_handle, uint8_t out_mac[6]);

/** 指定连接上输入报告格式（0x05 / 0x09）的 CCCD 是否已由主机开启。 */
bool ble_controller_input_notify_ready(uint16_t conn_handle, uint8_t report_format);

/** 指定连接的当前连接间隔（1.25ms 单位，4 = 5ms）。NS2 主机要求约 5ms
 * （约 200Hz 上报），间隔偏大时主机会连接、订阅但忽略输入报文；该值由
 * 主机下发的连接更新决定，控制器需允许亚规范间隔（sdkconfig 的
 * CONFIG_BT_CTRL_BLE_MIN_CONN_INTERVAL_ENABLE，默认开启）。 */
bool ble_controller_conn_itvl(uint16_t conn_handle, uint16_t *out_itvl);

/** 连接观测：当前间隔（1.25ms 单位）、协商后的 ATT MTU、通知投递失败计数
 *  与最近一次失败的返回码（0 = 未失败）。MTU < 66 时 63 字节输入通知发不
 *  出去，主机会表现为「已订阅但无输入」。任一指针可传 NULL。 */
bool ble_controller_conn_stats(uint16_t conn_handle, uint16_t *out_itvl, uint16_t *out_mtu,
                               uint32_t *out_tx_fail, int *out_tx_rc, bool *out_encrypted);

/** 最近一次真正投递的输入报文（63B，不含 Report ID），供 CLI 抓取线上内容。 */
bool ble_controller_last_input(uint16_t conn_handle, uint8_t report_format, uint8_t *out);

/** 发送 Input Report 0x05 / 0x09 通知到指定连接（未订阅时静默丢弃，同时刷新 READ 缓存）。 */
void ble_controller_notify_input_05(uint16_t conn_handle, const uint8_t report[63]);
void ble_controller_notify_input_09(uint16_t conn_handle, const uint8_t report[63]);

/** 发送指令应答帧（0x001E，需该主机已开 0x001F CCCD）到指定连接。 */
void ble_controller_notify_answer(uint16_t conn_handle, const uint8_t *frame, size_t len);

/** 停止全部广播实例（未配对空闲态不保持可发现广播，与真实手柄一致）。 */
void ble_controller_adv_stop(void);

/**
 * 以 31 字节原始载荷启动一个广播实例。addr 为 NULL 时用公共伪装地址
 * （Pro 单身份，实例 0 走扩展 PDU、实例 1 走 legacy PDU 的既有形态）；
 * 非 NULL 时以该静态随机地址广播（JoyCon 双身份各占一个实例，legacy PDU）。
 * instance 取 0/1；identity 由会话层显式给出（ns2_identity_t），传输层不再
 * 从地址反推——左右两只的地址最低位来自芯片，反推会认错身份。
 */
void ble_controller_adv_start(uint8_t instance, uint8_t identity,
                              const uint8_t payload[31], const uint8_t addr[6]);

/** 指定身份的广播实例是否在发（诊断用；Pro 两个实例任一在发即为真）。 */
bool ble_controller_adv_running(uint8_t identity);

/** 解析连接的本机地址（广播身份来源）：命中返回 true 并按 identity 输出。 */
bool ble_controller_conn_identity(uint16_t conn_handle, uint8_t *identity);

#ifdef __cplusplus
}
#endif

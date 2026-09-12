#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NimBLE 手柄外设传输层（ADR 0010）：NimBLE 生命周期、GATT 表、原始广播与通知发送。
 * 协议语义（广播内容、指令应答、会话状态）由 ble_session 决策，本模块只负责收发。
 * GATT 表按 controller.md §4 的句柄布局注册（含占位描述符对齐，见源内注释）。
 */

/** 初始化 NimBLE 并启动 host 任务；成功后栈在同步回调里触发发现广播。 */
esp_err_t ble_controller_start(void);

/** 是否处于 ACL 连接中。 */
bool ble_controller_connected(void);

/** 主动断开的 HCI 原因码（NimBLE hci_err 取值子集）。
 * 0x3E 连接建立失败会让手机等回连方按失败退避冷却，而非立刻重试。 */
#define BLE_CTL_DISCONNECT_USER_TERM 0x13
#define BLE_CTL_DISCONNECT_CONN_FAIL 0x3E

/** 主动断开当前连接并携带 HCI 断开原因；无连接或发起失败返回 false。 */
bool ble_controller_disconnect(uint8_t hci_reason);

/** 读取对端主机蓝牙地址（NimBLE 存储序，即显示序反转，与配对线格式一致）。 */
bool ble_controller_peer_mac(uint16_t conn_handle, uint8_t out_mac[6]);

/** 指定输入报告格式（0x05 / 0x09）的 CCCD 是否已由主机开启。 */
bool ble_controller_input_notify_ready(uint8_t report_format);

/** 发送 Input Report 0x05 / 0x09 通知（未订阅时静默丢弃，同时刷新 READ 缓存）。 */
void ble_controller_notify_input_05(const uint8_t report[63]);
void ble_controller_notify_input_09(const uint8_t report[63]);

/** 发送指令应答帧（0x001E，需主机已开 0x001F CCCD）。 */
void ble_controller_notify_answer(const uint8_t *frame, size_t len);

/** 停止广播（未配对空闲态不保持可发现广播，与真实手柄一致）。 */
void ble_controller_adv_stop(void);

/** 以 31 字节原始载荷启动通用可发现广播（ADV_IND）。 */
void ble_controller_advertise(const uint8_t payload[31]);

#ifdef __cplusplus
}
#endif

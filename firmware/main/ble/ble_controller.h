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

/** 指定输入报告格式（0x05 / 0x09）的 CCCD 是否已由主机开启。 */
bool ble_controller_input_notify_ready(uint8_t report_format);

/** 发送 Input Report 0x05 / 0x09 通知（未订阅时静默丢弃，同时刷新 READ 缓存）。 */
void ble_controller_notify_input_05(const uint8_t report[63]);
void ble_controller_notify_input_09(const uint8_t report[63]);

/** 发送指令应答帧（0x001A，需主机已开 0x001B CCCD）。 */
void ble_controller_notify_answer(const uint8_t *frame, size_t len);

/** 以 31 字节原始载荷启动通用可发现广播（ADV_IND）。 */
void ble_controller_advertise(const uint8_t payload[31]);

#ifdef __cplusplus
}
#endif

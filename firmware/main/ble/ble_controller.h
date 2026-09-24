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
 * NimBLE 手柄外设传输层：NimBLE 生命周期、GATT 表、原始广播与通知发送；
 * 协议语义由 ble_session 决策，本模块只负责收发。GATT 表按 docs/controller-switch2.md 的句柄布局注册。
 * 设备对外只有一台 Pro Controller 2（单身份、单条链路），广播默认用 legacy PDU。
 */

/** 起栈：初始化 NimBLE 并启动 host 任务（栈已在跑时直接返回）；
 *  成功后栈在同步回调里触发发现广播。 */
esp_err_t ble_controller_start(void);

/** 关栈：停广播、停 host 事件循环、关闭并反初始化控制器（射频与 modem 断电）。
 *  只能在非 NimBLE host 任务上调用——内部要等 host 任务退出，控制面服务任务即是；
 *  关掉后 ble_controller_start 可以再次把栈带起来。 */
esp_err_t ble_controller_stop(void);

/** BLE 栈是否在跑（控制器已使能）：关栈期间所有 NimBLE 入口都不可调用。 */
bool ble_controller_running(void);

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

/** 指定连接上输入报告通道（0x05 通用，或 0x09 专用）的 CCCD 是否已由主机开启。 */
bool ble_controller_input_notify_ready(uint16_t conn_handle, uint8_t report_format);

/** 主机在指定连接上订阅的专用输入通道句柄（0 = 未订阅）。主机在 0x000E
 *  句柄上按型号换 UUID，本设备按 Pro 的规格注册；串口 link 用它确认主机
 *  订的是哪一条通道。 */
bool ble_controller_input_priv_handle(uint16_t conn_handle, uint16_t *out_handle);

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

/** 发送输入报告通知到指定连接（0x05 走通用通道；0x09 走主机订阅的那个专用
 *  通道句柄），未订阅时静默丢弃，同时刷新 READ 缓存。 */
void ble_controller_notify_input_05(uint16_t conn_handle, const uint8_t report[63]);
void ble_controller_notify_input_09(uint16_t conn_handle, const uint8_t report[63]);

/** 只刷新 READ 缓存、不发通知（特性未启用的链路上也要保持快照新鲜）。 */
void ble_controller_store_input(uint16_t conn_handle, uint8_t report_format,
                                const uint8_t report[63]);

/** 发送指令应答帧（0x001E，需该主机已开 0x001F CCCD）到指定连接。 */
void ble_controller_notify_answer(uint16_t conn_handle, const uint8_t *frame, size_t len);

/** 停止全部广播实例（设备静默时不留可发现广播，与真实手柄一致）。 */
void ble_controller_adv_stop(void);

/** 停止指定身份在发的广播实例（Pro 两实例同址，一并停止）。 */
void ble_controller_adv_stop_identity(uint8_t identity);

/** 以 31 字节原始载荷启动一个广播实例：addr 为 NULL 用公共伪装地址，否则用该静态随机地址；
 *  instance 取 0/1，identity 由会话层显式给出（传输层不从地址反推身份）。 */
void ble_controller_adv_start(uint8_t instance, uint8_t identity,
                              const uint8_t payload[31], const uint8_t addr[6]);

/** 广播 PDU 形态（对账开关，不落盘）：auto 与 legacy 都是 legacy PDU
 *  （可连接 + 可扫描，主机只认这种）；extended 换成
 *  扩展 PDU 做反向验证——扩展实例在主机侧完全看不见。 */
typedef enum {
    BLE_CTL_ADV_PDU_AUTO = 0,
    BLE_CTL_ADV_PDU_LEGACY = 1,
    BLE_CTL_ADV_PDU_EXTENDED = 2,
} ble_ctl_adv_pdu_form_t;

void ble_controller_set_adv_pdu_form(uint8_t form);

/** 当前 PDU 形态（串口 `advpdu` 无参回显用）。 */
uint8_t ble_controller_adv_pdu_form(void);

/** 形态短名（串口回显）：auto / legacy / extended。 */
const char *ble_controller_adv_pdu_form_name(uint8_t form);

/** 指定身份的广播实例是否在发（诊断用；Pro 两个实例任一在发即为真）。 */
bool ble_controller_adv_running(uint8_t identity);

/** 解析连接的本机地址（广播身份来源）：命中返回 true 并按 identity 输出。 */
bool ble_controller_conn_identity(uint16_t conn_handle, uint8_t *identity);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 局域网调试会话（`REMAPAD_NETLOG=OFF` 可整体裁掉，默认编入）：WiFi STA 连上
 * AP 后，单个 UDP 端口（默认 9999）承载与串口同一模型的数据——桥接帧（UDP
 * 网络手柄与 OTA 升级，amiibo 上传仍只走串口）与 CLI 文本靠帧同步字区分，日志
 * 与 CLI 回复抄送到最后说话的 PC。
 * host 模式下 USJ 让给手柄、UART0 没接适配器时，这是唯一的远端观测与输入
 * 通道。凭据可保存进 NVS 供界面与 CLI 手动起会话（开机不自动连）；断线按
 * 退避节奏重试（3 秒 → 30 秒），累计 10 次失败自动关闭会话（节奏见
 * netlog_retry.h）。目标 IP 不落盘，由收到的第一个报文自学。
 */

#define NETLOG_PORT_DEFAULT 9999
#define NETLOG_SSID_MAX 33
#define NETLOG_PASS_MAX 65
#define NETLOG_HOST_MAX 40

/** 连上 AP 并开始收发；host 传 NULL 时目标自学习（等 PC 先发一个报文），
 *  port 取 0 时用默认值。返回 ESP_ERR_INVALID_STATE 表示会话已在跑。 */
esp_err_t netlog_start(const char *ssid, const char *password, const char *host, uint16_t port);

/** 停止会话并关掉 WiFi 射频；可在 UDP 收任务自身上下文里调用（netlog off）。 */
void netlog_stop(void);

/** 把 WiFi 凭据写进用户设置并立即落盘；不碰在跑的会话，连接由界面或 CLI 手动开。 */
esp_err_t netlog_save_wifi(const char *ssid, const char *password);

/** 起后台任务扫一圈周围 AP（约 2-15 秒）：结果按 RSSI 降序存进模块并逐条打进日志，
 *  `netlog scanlist` 可随时重印；会话开着时扫描压住断线重连、扫完自动回连。
 *  *count 回当前已存条数。只在 WiFi 驱动就绪后可用（INVALID_STATE 表示还没就绪）。 */
esp_err_t netlog_scan(int *count);

/** 取扫描结果第 index 条的一行摘要（序号/ssid/bssid/信道/RSSI/加密模式）；越界返回 false。 */
bool netlog_scan_line(int index, char *out, size_t cap);

/** 擦掉 NVS 里的射频校准数据，随后应重启：下次起 WiFi 强制全量重校。驱动半初始化
 *  期间写下的坏校准会被局部校准反复复用，症状是任何 AP 都解不出本机发射帧。 */
esp_err_t netlog_phy_reset(void);

/** 读/写 STA 最大发射功率（0.25dBm 单位，受国家码限幅约束）。只在会话运行中有效
 *  （INVALID_STATE 表示 WiFi 未起）；会话重开后回到国家默认值，供供电/功率判别实验用。 */
esp_err_t netlog_get_tx_power(int8_t *quarter_dbm);
esp_err_t netlog_set_tx_power(int8_t quarter_dbm);

/** 主动断开触发一轮新的认证-关联（再失败回到退避节奏，连上即清零失败计数）；会话未运行返回 INVALID_STATE。 */
esp_err_t netlog_reconnect(void);

/** 会话是否开着（含 WiFi 连接中）。 */
bool netlog_running(void);

/** WiFi 是否已拿到 IP（此后汇点的发送开始落地）。 */
bool netlog_connected(void);

/** 桥接帧是否在位：会话连着且最近仍有报文往来（反馈帧跟着活跃的输入端走）。 */
bool netlog_bridge_active(void);

/** 设备自己的 "ip:port"（连接后有效），未连接时写 "--"。 */
void netlog_addr_text(char *out, size_t cap);

/** 当前 WiFi 信号强度（dBm，负值）；未连接返回 0。 */
int netlog_rssi(void);

/** 当前状态一行（state/ssid/dest/收发计数），给 CLI 回读与 PC 设置页解析。 */
void netlog_status_line(char *out, size_t cap);

/** console_out 汇点：把一段已格式化文本经 UDP 发出；未连接或发送重入时静默丢弃。 */
void netlog_sink_write(const char *text, size_t len);

#ifdef __cplusplus
}
#endif

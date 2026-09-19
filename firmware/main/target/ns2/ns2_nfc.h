#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 NFC 命令通路（Command 0x01）的标签模拟：板卡没有 NFC 前端，主机读到
 * 的「标签」由预置的 NTAG215 镜像（amiibo dump，540 字节）在软件里扮演。
 *
 * 各子命令的应答体按 ndeadly/switch2_controller_research 的实机抓包钉住
 * （controller.md「NFC 与 Amiibo 数据交互协议规范」）：
 *   0x03 开轮询 / 0x04 关轮询 / 0x06 触发读卡 / 0x08 提交写卡 / 0x14 装载写
 *   缓冲：只回帧头 ACK；0x05 取卡信息：63 字节体（状态前缀 + 7B UID + 补零，
 *   无镜像时全零）；0x0C 取 NFC 状态：4 字节体（抓包原值）；0x15 取读缓冲：
 *   状态 + 数据长度（u16 LE）+ 最多 70 字节镜像数据。
 * 0x05 无镜像的应答形态（按全零体实现）与 0x05 一族蓝牙下的帧头（抓包来自
 * USB，蓝牙形态按「ACK 字节按子命令固定」外推）未经实机对账，真机不符时先从这里换起。
 *
 * 读卡状态机（0x05 体首字节与 Report 0x09 的 NFC 状态字节同源，2026-09-19
 * 实机对账验证到整机抽块完成）：卡片在场 0x09（抓包原值）→ 0x06 触发后
 * 0x14（读取中）→ 读卡耗时（约 30ms）后 0x15（数据就绪）→ 主机连抽缓冲
 * （期间状态不变）→ 拉到长度 0 结束，状态报「读取结束」值（串口可调，
 * 默认 0x00）。0x02-0x07 实测扫值主机不据此抽块。
 *
 * 镜像由 amiibo 存储层（amiibo_store）或串口测试预置；主机写卡的存档经
 * 写回回调交给存储层落盘。Report 0x09 的 NFC 状态字节与 0x05 体首字节同源
 * （见上）。
 */

/** NTAG215 用户区完整镜像长度（135 页 × 4B，amiibo dump 通行尺寸）。 */
#define NS2_NFC_TAG_SIZE 540u
/** NTAG215 厂商签名（READ_SIG 页）长度：572 字节 dump 把它附在镜像尾部。 */
#define NS2_NFC_SIG_SIZE 32u
/** 带签名的整份 dump（540 镜像 + 32 签名）；上传与槽位记录按此长度。 */
#define NS2_NFC_IMAGE_MAX (NS2_NFC_TAG_SIZE + NS2_NFC_SIG_SIZE)
/** 读缓冲区的读卡结果头长度；标签数据从偏移 60 开始——抓包里偏移 70 的
 *  应答数据以标签第 10 字节开头（LOCK + CC + 页 4）钉住该布局。 */
#define NS2_NFC_BUFFER_HEADER 60u
/** 读缓冲区总长（头 + 标签镜像）。 */
#define NS2_NFC_BUFFER_TOTAL (NS2_NFC_BUFFER_HEADER + NS2_NFC_TAG_SIZE)
/** 0x15 单次读缓冲的数据上限（实机抓包的块长）。 */
#define NS2_NFC_READ_CHUNK_MAX 70u
/** 0x05 应答体长度（抓包定长，尾部补零）。 */
#define NS2_NFC_TAG_INFO_BODY_LEN 63u
/** 0x0C 应答体（NFC 控制器状态，抓包原值）。 */
#define NS2_NFC_STATUS_BODY {0x61, 0x12, 0x50, 0x0D}

/** 清空镜像与轮询状态，解除写回回调（主机端用例的隔离入口）。 */
void ns2_nfc_reset(void);

/** 预置标签镜像：len = 540（不带厂商签名，签名区清零）或 572（镜像 + 尾部
 *  32 字节厂商签名）；传 NULL/0 清除。 */
esp_err_t ns2_nfc_stage(const uint8_t *data, size_t len);

/** 当前是否已预置镜像。 */
bool ns2_nfc_ready(void);

/** 读镜像一段（偏移越界部分不计入），返回实际读取字节数。 */
size_t ns2_nfc_read(uint32_t offset, uint8_t *out, size_t len);

/** 镜像里的 7 字节 UID（NTAG215 页 0-2 的 UID 字段）；未预置返回 false。 */
bool ns2_nfc_uid(uint8_t out[7]);

/** 串口 / CLI 手动开关：无主机时让标签入场/离场，验证状态字节与读卡流程。 */
void ns2_nfc_set_polling(bool on);
bool ns2_nfc_polling(void);

/** 输入报告的 NFC 状态字节（与 0x05 体首字节同源）：场开无卡 0x01、卡片在场
 *  0x09、0x06 触发后 0x14（读取中）→ 30ms 后 0x15（就绪）、抽块结束报
 *  「读取结束」值（ns2_nfc_set_drained_state 可调，默认 0x00）；关轮询 0x00。 */
uint8_t ns2_nfc_report_state(void);

/** 手动钉住报告状态值（串口 `amiibo state <n>` 对账用）：0 回自动，
 *  非 0 原样钉住（如 0x14/0x15）。 */
void ns2_nfc_set_report_stage(uint8_t stage);

/** 设置「读取结束」（EOF 探测后）的报告状态值，扫正确的完成信号用，
 *  默认 0x00 = Idle。 */
void ns2_nfc_set_drained_state(uint8_t state);

/** 读缓冲 60 字节头区的填充模式：1 = 按抓包重构的结构填充（默认）：
 *      31 02 00 00 00 | 01 02 00 07 | UID(7B) | 00 00 00 00 | 厂商签名(32B)
 *      | 00 | 3B 3C 77 78 86 | 00 00
 *  结构来自 Switch 1 MCU 时代的读卡响应（Poohl/joycontrol mcu.md read1），
 *  尾部 3B 3C 77 78 86 00 00 与 NS2 抓包 0x06 读触发载荷尾部同串；签名取
 *  572 字节 dump 尾部（未带签名时为全零）。0 = 全零头（对账基线）。
 *  串口 `amiibo hdr 0|1` 实时可切。 */
void ns2_nfc_set_header_mode(uint8_t mode);

/** 当前头区填充模式（0/1）。 */
uint8_t ns2_nfc_header_mode(void);

/** 抽块结束（EOF 探测）后主动推送给主机的完成事件：Switch 1 MCU 时代的读卡
 *  流程以控制器主动推 read3 状态帧收尾（`09 | 31 04 00 00 00 | 01 01 02 00 07 |
 *  UID`），NS2 的 0x05 体把 `31 XX` 标记压掉了，但完成信号可能仍要主动推。
 *  推送模式（串口 `amiibo push <n>` 实时可切，扫正确形态用）：
 *    0 = 不推（默认）；1 = 0x05 体带 31 04 完成标记（MCU read3 形态）；
 *    2 = 0x05 普通卡信息体；3 = 0x06 空体；4 = 0x15 空体（00 00 00）。 */
void ns2_nfc_set_push_mode(uint8_t mode);

/** 当前推送模式（0-4）。 */
uint8_t ns2_nfc_push_mode(void);

/** 待推的完成事件（ble_session 在 0x15 EOF 应答后补发一帧）。 */
typedef struct {
    uint8_t subcmd;
    uint8_t body[NS2_NFC_TAG_INFO_BODY_LEN];
    size_t body_len;
} ns2_nfc_event_t;

/** 弹出待推事件（有则填充 out 并返回 true；事件只弹一次）。 */
bool ns2_nfc_pop_event(ns2_nfc_event_t *out);

/** Command 0x01 分发入口（ble_session 调用）：按子命令填应答帧（帧头 + 体），
 *  返回应答总长度；参数不完整时只回帧头。req 是完整指令帧。 */
size_t ns2_nfc_on_command(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp,
                          size_t cap);

/** 应答帧头的 Status/ACK 字节按子命令取抓包值：0x0C/0x15 是 10/78，其余已见
 *  子命令（0x03/0x04/0x05/0x06/0x08/0x14）是 00/F8——USB 抓包里 LED/电量类
 *  10/78、NFC/初始化类 00/F8，按子命令固定的解读与两条传输层的抓包都自洽。
 *  命中返回 true 并写出两个字节；未知子命令返回 false，沿用通用帧头。 */
bool ns2_nfc_response_ack(uint8_t subcmd, uint8_t *status, uint8_t *ack);

/** 写卡提交回调：0x08 提交后收到完整镜像；返回 false 记录落盘失败（镜像
 *  本身已更新，主机侧不受影响）。 */
typedef bool (*ns2_nfc_write_sink_fn)(const uint8_t *data, size_t len, void *user);

/** 注册写卡落盘入口（amiibo 存储层在初始化时接线）；fn 传 NULL 解除。 */
void ns2_nfc_set_write_sink(ns2_nfc_write_sink_fn fn, void *user);

#ifdef __cplusplus
}
#endif

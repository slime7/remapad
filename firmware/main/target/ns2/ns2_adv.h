#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 手柄广播载荷（controller.md §2.1，纯逻辑，可主机端测试）：
 * 31 字节 = BLE Flags 3B + 厂商数据 28B。三种形态只差两处——厂商数据
 * 偏移 0x0B 的状态字节与偏移 0x0C-0x11 的目标主机地址（反序）：
 *
 *   - 发现广播：不带主机地址，状态位 0x00；
 *   - 回连广播：带主机地址，状态位 0x00；
 *   - 唤醒广播：带主机地址，状态位 0x81。
 *
 * 状态位是主机唯一的唤醒判据：已配对设备在未连接期间常驻唤醒形态——主机
 * 醒着但停在任意页面时也只认 0x81（0x00 回连形态不被采纳），这是「只有主机
 * 停在配对页面才连得上」的解法；未配对或处于配对流程时发发现广播等主机
 * 搜索。策略与取舍见 ADR 0024。
 */

#define NS2_ADV_PAYLOAD_LEN 31
/** 厂商数据内偏移：状态字节 0x0B、主机地址 0x0C（载荷内 +5）。 */
#define NS2_ADV_MFR_STATUS_OFFSET 0x0B
#define NS2_ADV_MFR_HOST_MAC_OFFSET 0x0C

#define NS2_ADV_STATUS_NORMAL 0x00
#define NS2_ADV_STATUS_WAKE 0x81

typedef enum {
    NS2_ADV_DISCOVERY = 0, /**< 标准发现广播：等待主机搜索/首次配对。 */
    NS2_ADV_RECONNECT = 1, /**< 已配对回连广播：等待主机回连。 */
    NS2_ADV_WAKE = 2,      /**< 唤醒广播：请休眠中的主机立即醒来。 */
} ns2_adv_mode_t;

/** 广播形态决策：配对流程中或未配对的身份发发现广播（未配对身份绝不发唤醒
 *  广播——不允许把主机从休眠里叫醒）；已配对身份发 steady，默认 NS2_ADV_WAKE，
 *  只有实机 A/B 对账时才用 NS2_ADV_RECONNECT 退回 0x00 回连形态。 */
ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool pairing_requested,
                                   ns2_adv_mode_t steady);

/** L+R 组合确认的重试间隔（微秒）：主机把两只 Joy-Con 认成一对靠 L 与 R 同时
 *  按下，拿到凭证之前按这个节奏重发。 */
#define NS2_ADV_LR_RETRY_US (3 * 1000 * 1000LL)

/** 单次 L+R 的按住时长（毫秒）：15 ms 上报节奏下约八帧，主机不会漏看。 */
#define NS2_ADV_LR_HOLD_MS 120

/** L+R 组合确认的复注入计时（本机时基，微秒）。 */
typedef struct {
    int64_t next_us; /**< 允许注入的时刻；0 = 立即可注入。 */
} ns2_adv_lr_timer_t;

/** 是否该注入 L+R：未配对（该形态凭证未拿齐）且左右两只都已就绪（收到
 *  0x0c/0x04、输入被主机采用）时立即注入一次，之后每 NS2_ADV_LR_RETRY_US
 *  重试；已配对或未就绪时从不注入。命中时把下次时刻推后，调用方据此注入。 */
bool ns2_adv_lr_step(ns2_adv_lr_timer_t *timer, bool paired, bool both_ready,
                     int64_t now_us);

/** 复位计时（切换身份、重进配对流程）：下一次就绪即注入，不再等间隔。 */
void ns2_adv_lr_reset(ns2_adv_lr_timer_t *timer);

/** 休眠链路判据：主机已订阅输入、却始终没发 0x0c/0x04（启用特性）——输入
 *  报文被采用的前提是特性启用（参考实现把整个上报流押在它上，DEV_READY），
 *  与连接间隔无关（实测 itvl=4 但未启用的链路按键同样无效）。「已连接、已
 *  订阅、上报在发，但按键无反应」正是这个形态（握把页快捷回连即如此）。
 *  会话层据此驱动休眠看门狗。 */
bool ns2_adv_dormant_link(bool subscribed, bool features_enabled);

/** 生成 31 字节广播载荷：pid 为本机型号 ID（Pro 0x2069 / JoyCon 2 0x2067、
 *  0x2066），host_mac 为主机地址（NimBLE 存储序，即显示序反转，与配对线
 *  格式一致）。发现形态忽略 host_mac 并把地址填零；回连/唤醒形态在
 *  host_mac 为 NULL 时退化为发现形态。 */
void ns2_adv_payload(uint8_t out[NS2_ADV_PAYLOAD_LEN], uint16_t pid,
                     ns2_adv_mode_t mode, const uint8_t host_mac[6]);

#ifdef __cplusplus
}
#endif

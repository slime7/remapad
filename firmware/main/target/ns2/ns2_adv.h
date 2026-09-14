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
 * 状态位是主机唯一的唤醒判据：真机抓包里回连形态恒为 0x00，只有用户按键
 * 触发的唤醒突发（约 2 秒）才发 0x81，超时立即回到 0x00。
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

/**
 * 唤醒窗口：真机按键唤醒只发约 2 秒的 0x81 广播，之后立即回到 0x00 的
 * 回连形态——0x81 是主机唯一的「醒来」判据，长窗口或常驻会让休眠中的
 * 主机反复被叫醒。窗口在本机时基上计时，连接建立时立刻关闭。
 */
typedef struct {
    int64_t until_us; /**< 0 = 窗口未开；非 0 = 截止时刻。 */
} ns2_adv_wake_window_t;

/** 打开（或顺延）唤醒窗口：从 now_us 起 length_us 内发唤醒广播。 */
void ns2_adv_wake_window_open(ns2_adv_wake_window_t *win, int64_t now_us, int64_t length_us);

/** 立即关闭唤醒窗口：主机已回连或要明确回到回连形态时用。 */
void ns2_adv_wake_window_close(ns2_adv_wake_window_t *win);

/** 窗口是否仍然有效：到达截止时刻即失效。 */
bool ns2_adv_wake_window_active(const ns2_adv_wake_window_t *win, int64_t now_us);

/** 广播形态决策：未配对的身份绝不发唤醒广播（不允许把主机从休眠里叫醒），
 *  发发现广播等主机搜索；已配对身份在唤醒窗口内发唤醒广播，其余时间发
 *  回连广播等主机主动回连。 */
ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool in_wake_window);

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

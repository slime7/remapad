#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 手柄广播载荷（协议见 docs/controller-switch2.md，纯逻辑，可主机端测试）：
 * 31 字节 = BLE Flags 3B + 厂商数据 28B，三种形态只差状态字节（0x81 会唤醒休眠主机、0x00 不会）
 * 与目标主机地址（反序）两处：发现广播不带地址，回连与唤醒广播带地址。
 * 设备只在被请求后按组装信号广播（窗口时长 + 唤醒突发时长）。
 */

#define NS2_ADV_PAYLOAD_LEN 31
/** 厂商数据内偏移：状态字节 0x0B、主机地址 0x0C（载荷内 +5）。 */
#define NS2_ADV_MFR_STATUS_OFFSET 0x0B
#define NS2_ADV_MFR_HOST_MAC_OFFSET 0x0C

#define NS2_ADV_STATUS_NORMAL 0x00
#define NS2_ADV_STATUS_WAKE 0x81

typedef enum {
    NS2_ADV_OFF = 0,       /**< 静默：不发广播（没被请求连接时的形态）。 */
    NS2_ADV_DISCOVERY = 1, /**< 标准发现广播：等待主机搜索/首次配对。 */
    NS2_ADV_RECONNECT = 2, /**< 已配对回连广播：等待主机回连。 */
    NS2_ADV_WAKE = 3,      /**< 唤醒广播：请休眠中的主机立即醒来。 */
} ns2_adv_mode_t;

/** 广播窗口：设备只在被显式请求后的一段时间内广播，不开机不发信号。 */
typedef struct {
    int64_t opened_at_us; /**< 窗口开启时刻（微秒）；0 = 没有窗口。 */
    int64_t until_us;     /**< 到期时刻（本机时基微秒）；0 = 没有窗口。 */
    int64_t burst_us;     /**< 开窗后先发唤醒形态的时长（0 = 全程回连形态）。 */
} ns2_adv_window_t;

/** 连接窗口时长：主机没在这段时间内连上就静默，想重试再按一次。 */
#define NS2_ADV_CONNECT_WINDOW_US (30 * 1000 * 1000LL)

/** 唤醒突发时长（3秒）：信号搜索启动时前 3 秒发 0x81 唤醒休眠主机，随后切为 0x00 回连。 */
#define NS2_ADV_WAKE_BURST_US (3 * 1000 * 1000LL)

/** 唤醒窗口时长：唤醒突发只有约 2 秒，主机扫描窗口远长于它，太短会错过。 */
#define NS2_ADV_WAKE_WINDOW_US (10 * 1000 * 1000LL)

/** 广播信号组装参数：一次「发什么、发多久」的全部决策点。各种信号情况不再
 *  各设入口，按情况填参数经 ns2_adv_window_open 组装开窗；以后新增信号情况
 *  只需组合参数调用，不改形态决策逻辑。 */
typedef struct {
    int64_t duration_us; /**< 窗口总时长：到期静默。 */
    int64_t burst_us;    /**< 前置唤醒突发时长：开窗后先发 0x81 叫醒休眠主机，
                          *  随后转 0x00 回连；0 = 不带突发，全程回连形态。 */
} ns2_adv_signal_t;

/** 信号搜索：窗口前段发唤醒突发叫醒休眠主机，随后回连等醒着的主机连回来。
 *  连接键、开机信号搜索与 Dock 点击共用。 */
#define NS2_ADV_SIGNAL_SEARCH \
    ((const ns2_adv_signal_t){.duration_us = NS2_ADV_CONNECT_WINDOW_US, \
                              .burst_us = NS2_ADV_WAKE_BURST_US})

/** 断连回连：主机睡下链路断开后自动开的回连窗口，全程回连形态不带唤醒突发
 *  ——链路断开可能正是用户主动休眠主机，回连不得把它立刻叫醒。 */
#define NS2_ADV_SIGNAL_RECONNECT \
    ((const ns2_adv_signal_t){.duration_us = NS2_ADV_CONNECT_WINDOW_US, .burst_us = 0})

/** 唤醒：HOME 唤醒窗口，整窗发唤醒形态把休眠主机叫起来。 */
#define NS2_ADV_SIGNAL_WAKE \
    ((const ns2_adv_signal_t){.duration_us = NS2_ADV_WAKE_WINDOW_US, \
                              .burst_us = NS2_ADV_WAKE_WINDOW_US})

/** 开窗（重新计时）：按组装信号设定窗口时长与唤醒突发，重复开窗按新信号
 *  换算并重新计时。 */
void ns2_adv_window_open(ns2_adv_window_t *win, const ns2_adv_signal_t *signal,
                         int64_t now_us);

/** 收窗：主机连上、用户停止广播、或窗口到期后调用。已收窗时无副作用。 */
void ns2_adv_window_close(ns2_adv_window_t *win);

/** 窗口是否仍然有效（没有窗口或已到期都返回 false）。 */
bool ns2_adv_window_active(const ns2_adv_window_t *win, int64_t now_us);

/** 当前该发的广播形态（纯逻辑，主机端用例钉住）：配对流程中恒发发现广播；
 *  没有窗口就静默；窗口内未配对发发现广播，已配对按组装参数分时发唤醒突发与回连形态。 */
ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool pairing_requested,
                                   const ns2_adv_window_t *window, int64_t now_us);

/** 调试页 HOME 按键的动作（实体手柄语义）。 */
typedef enum {
    NS2_HOME_INJECT = 0, /**< 主机在线：HOME 就是主页键，注入按键即可。 */
    NS2_HOME_WAKE = 1,   /**< 未连接：按键到不了主机，改走唤醒窗口。 */
} ns2_home_action_t;

/** 主机在线与否决定 HOME 按键的动作：醒着当主页键、睡眠当唤醒键。 */
ns2_home_action_t ns2_adv_home_action(bool connected);

/** HOME 按键的边沿状态：实体手柄的 HOME 在按下那一刻起作用（主机在线时上报
 *  主页键、不在线时开唤醒窗口），一直按住不重复触发。 */
typedef struct {
    bool down; /**< 上一拍 HOME 是否按下。 */
} ns2_adv_home_key_t;

/** 推进一步：本次是「刚按下」（上升沿）时返回 true。 */
bool ns2_adv_home_key_step(ns2_adv_home_key_t *key, bool pressed);

/** 回连/唤醒广播要携带的主机地址（纯逻辑，主机端用例钉住）：优先「最近一次
 *  NS2 会话记录到的对端地址」——配对交换给的是主机两条只差一位（末字节 ±1）
 *  的地址，凭证里存的那条未必是主机连接时在用的那条，连接对端地址才是；其次
 *  取由新到旧的第一条可用凭证；两者都没有可用地址时返回 NULL，调用方据此
 *  退化为发现广播（绝不发全零地址的唤醒广播）。 */
const uint8_t *ns2_adv_choose_host_mac(const uint8_t *recorded,
                                       const uint8_t *const creds[], size_t cred_count);

/** 休眠链路判据：主机已订阅输入、却始终没发 0x0c/0x04（启用特性）——输入
 *  报文被采用的前提是特性启用（参考实现把整个上报流押在它上，DEV_READY），
 *  与连接间隔无关（itvl=4 但未启用的链路按键同样无效）。「已连接、已
 *  订阅、上报在发，但按键无反应」正是这个形态（握把页快捷回连即如此）。
 *  会话层据此驱动休眠看门狗。 */
bool ns2_adv_dormant_link(bool subscribed, bool features_enabled);

/** 主机注册证据（纯逻辑，主机端用例钉住）：对端地址命中凭证、私有配对握手走完、
 *  或主机已在链路上启用特性——三者任一条成立即算注册；
 *  「已订阅但没启用特性」的快捷回连不算注册。 */
bool ns2_adv_host_registered(bool addr_matched, bool pair_handshake_done,
                             bool features_enabled);

/** 关栈判据（纯逻辑，主机端用例钉住）：完全静默——没有连接、没有广播窗口、
 *  不在配对流程——才允许关掉 BLE 控制器省电；窗口或配对流程进行中关栈会让
 *  主机再也连不上。 */
bool ns2_adv_stack_idle(bool connected, bool pairing, bool window_active);

/** 生成 31 字节广播载荷：pid 为本机型号 ID（Pro Controller 2 = 0x2069），
 *  host_mac 为主机地址（NimBLE 存储序，即显示序反转，与配对线格式一致）。
 *  发现形态与静默忽略 host_mac 并把地址填零；回连/唤醒形态在 host_mac 为
 *  NULL 时退化为发现形态。 */
void ns2_adv_payload(uint8_t out[NS2_ADV_PAYLOAD_LEN], uint16_t pid,
                     ns2_adv_mode_t mode, const uint8_t host_mac[6]);

#ifdef __cplusplus
}
#endif

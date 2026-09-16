#pragma once

#include <stdbool.h>
#include <stddef.h>
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
 * 状态位是主机唯一的唤醒判据：0x81 会把休眠中的主机叫起来，0x00 不会。
 * 设备与真机一样不主动发信号：上电、断连（主机睡下）都回到静默，只有用户
 * 按连接键打开连接窗口、或按 HOME 打开唤醒窗口时才广播——连接窗口内已配对
 * 身份发回连形态（主机醒着会自己按它连回来，实机：停在首页、顺序页或刚从
 * 待机醒来都会连），未配对身份发发现广播等主机搜索；唤醒窗口内才发唤醒形态，
 * 把睡下的主机叫起来。策略与取舍见 ADR 0038。
 */

#define NS2_ADV_PAYLOAD_LEN 31
/** 厂商数据内偏移：状态字节 0x0B、主机地址 0x0C（载荷内 +5）。 */
#define NS2_ADV_MFR_STATUS_OFFSET 0x0B
#define NS2_ADV_MFR_HOST_MAC_OFFSET 0x0C

#define NS2_ADV_STATUS_NORMAL 0x00
#define NS2_ADV_STATUS_WAKE 0x81

typedef enum {
    NS2_ADV_OFF = 0,       /**< 静默：不发广播（没被请求连接的真机形态）。 */
    NS2_ADV_DISCOVERY = 1, /**< 标准发现广播：等待主机搜索/首次配对。 */
    NS2_ADV_RECONNECT = 2, /**< 已配对回连广播：等待主机回连。 */
    NS2_ADV_WAKE = 3,      /**< 唤醒广播：请休眠中的主机立即醒来。 */
} ns2_adv_mode_t;

/** 广播窗口的打开来源：决定窗口内已配对身份发哪一种形态。 */
typedef enum {
    NS2_ADV_REQ_CONNECT = 0, /**< 连接键：屏幕「连接」按钮、PWR 长按 3 秒。 */
    NS2_ADV_REQ_WAKE = 1,    /**< 唤醒键：调试页 HOME 在未连接时按下。 */
} ns2_adv_request_t;

/** 广播窗口：设备只在被显式请求后的一段时间内广播，真机不开机不发信号。 */
typedef struct {
    ns2_adv_request_t request; /**< 最近一次打开窗口的请求。 */
    int64_t until_us;          /**< 到期时刻（本机时基微秒）；0 = 没有窗口。 */
} ns2_adv_window_t;

/** 连接键窗口时长：主机没在这段时间内连上就静默，想重试再按一次。 */
#define NS2_ADV_CONNECT_WINDOW_US (30 * 1000 * 1000LL)

/** 唤醒窗口时长：真机唤醒突发只有约 2 秒，主机扫描窗口远长于它，太短会错过。 */
#define NS2_ADV_WAKE_WINDOW_US (10 * 1000 * 1000LL)

/** 开窗（重新计时）：重复请求不会把窗口算短，按请求来源取对应时长。 */
void ns2_adv_window_open(ns2_adv_window_t *win, ns2_adv_request_t request,
                         int64_t now_us);

/** 收窗：主机连上、用户停止广播、或窗口到期后调用。已收窗时无副作用。 */
void ns2_adv_window_close(ns2_adv_window_t *win);

/** 窗口是否仍然有效（没有窗口或已到期都返回 false）。 */
bool ns2_adv_window_active(const ns2_adv_window_t *win, int64_t now_us);

/** 当前该发的广播形态（纯逻辑，主机端用例钉住）：
 *  - 配对流程中恒发发现广播——要配的是新主机，不能带着旧主机的地址广播；
 *  - 没有窗口就静默（NS2_ADV_OFF），设备不被请求连接时不发信号；
 *  - 连接窗口内：已配对发回连形态（醒着的主机自己连回来），未配对发发现
 *    广播（配对键语义，等主机搜索）；
 *  - 唤醒窗口内：已配对发唤醒形态把休眠主机叫起来；未配对没有主机可唤醒，
 *    退化为发现广播等主机来配。 */
ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool pairing_requested,
                                   const ns2_adv_window_t *window, int64_t now_us);

/** 调试页 HOME 按键的动作（实体手柄语义）。 */
typedef enum {
    NS2_HOME_INJECT = 0, /**< 主机在线：HOME 就是主页键，注入按键即可。 */
    NS2_HOME_WAKE = 1,   /**< 未连接：按键到不了主机，改走唤醒窗口。 */
} ns2_home_action_t;

/** 主机在线与否决定 HOME 按键的动作：醒着当主页键、睡眠当唤醒键。 */
ns2_home_action_t ns2_adv_home_action(bool connected);

/** 回连/唤醒广播要携带的主机地址（纯逻辑，主机端用例钉住）：优先「最近一次
 *  NS2 会话记录到的对端地址」——配对交换给的是主机两条只差一位（末字节 ±1）
 *  的地址，凭证里存的那条未必是主机连接时在用的那条，连接对端地址才是；其次
 *  取由新到旧的第一条可用凭证；两者都没有可用地址时返回 NULL，调用方据此
 *  退化为发现广播（绝不发全零地址的唤醒广播）。 */
const uint8_t *ns2_adv_choose_host_mac(const uint8_t *recorded,
                                       const uint8_t *const creds[], size_t cred_count);

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
 *  格式一致）。发现形态与静默忽略 host_mac 并把地址填零；回连/唤醒形态在
 *  host_mac 为 NULL 时退化为发现形态。 */
void ns2_adv_payload(uint8_t out[NS2_ADV_PAYLOAD_LEN], uint16_t pid,
                     ns2_adv_mode_t mode, const uint8_t host_mac[6]);

#ifdef __cplusplus
}
#endif

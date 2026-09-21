#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_adv.h"
#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 手柄会话：广播策略、连接初始化时序与指令分发（协议见 docs/controller-switch2.md）。
 * 传输细节由 ble_controller 承载；设备对外只有一台 Pro Controller 2（单身份、单报告格式、单条会话）。
 * 广播只由用户动作打开：上电与断连静默，连接键开连接窗口、HOME 开唤醒窗口，窗口到期或主机连上即关闭。
 */

/** host 同步完成（栈就绪）：记录自身 MAC，不启动广播（等用户按连接键）。 */
void ns2_session_on_sync(const uint8_t own_mac[6]);

/** ACL 连接建立（identity 为该连接呈现的手柄身份）。 */
void ns2_session_on_connect(uint16_t conn_handle, uint8_t identity);

/** 连接建立失败：按当前窗口同步广播，不影响会话状态。 */
void ns2_session_on_connect_fail(void);

/** 记录一次主机协议活动（ATT 读写/订阅），刷新所在连接的空闲计时。 */
void ns2_session_touch(uint16_t conn_handle);

/** 指定连接是否已超时无活动：连接中、握手未完成且超过空闲时限。
 * 供周期检查断开手机/PC 等只连不聊的回连方；主机初始化毫秒级到达，不受影响。 */
bool ns2_session_conn_idle_expired(uint16_t conn_handle);

/** 周期任务（1s，ble_controller 空闲定时器驱动）：固件假升级会话超时收尾。 */
void ns2_session_tick(void);

/** 断连（主机睡眠 / 移开）：复位该连接的会话并停止广播——没有窗口与配对
 *  流程就回到静默，等下一次连接键或唤醒键。 */
void ns2_session_on_disconnect(uint16_t conn_handle, uint8_t identity);

/** Command 通道（0x0014）写入：8 字节帧头 + 应答体，BLE 传输层。 */
void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport,
                            uint16_t conn_handle);

/** 震动通道（0x0012）写入：Output Report 0x02。解析成结构化事件交给反馈监听者。 */
void ns2_session_on_output(const uint8_t *data, size_t len, uint16_t conn_handle);

/** 复合输出通道（0x0016）写入：震动参数 + 指令帧。 */
void ns2_session_on_composite(const uint8_t *data, size_t len, uint16_t conn_handle);

/** 固件升级数据块（0x0018 WRITE NO RSP）：假升级会话入口——按记录拼接
 * 0x0d/0x04 命令帧，帧凑齐即按指令通道的格式应答，静默超时后递增上报版本
 * 并落盘（假装升级到新版本）。 */
void ns2_session_on_fw_upgrade(const uint8_t *data, size_t len, uint16_t conn_handle);

/** 升级帧的应答体（默认空体）：主机更新流程无公开文档，串口 fwack 现场替换做 A/B。 */
void ns2_session_set_fw_ack_body(const uint8_t *body, size_t len);

/** 读回当前升级帧应答体（写入 out，返回写入字节数）。 */
size_t ns2_session_fw_ack_body(uint8_t *out, size_t cap);

/** 更新应用后上报给主机的版本（默认 9.9.9，主机据此判断还要不要再推一次）。 */
void ns2_session_set_fw_post_version(const uint8_t ver[3]);
void ns2_session_fw_post_version(uint8_t out[3]);

/** 主机更新收尾（0x0d/0x07）时是否重启伪装「已升级」：默认关闭——重启
 *  会被主机当成更新没生效而重推整包，形成推包与重启的循环。武装是一次性的：
 *  触发后自动撤防，串口 fwapply on|off 控制。 */
void ns2_session_set_fw_restart_armed(bool armed);
bool ns2_session_fw_restart_armed(void);

/** 特性掩码 bit5（触觉震动）是否在任一活跃会话开启，影响 0x09 状态标志字节。 */
bool ns2_session_rumble_enabled(void);

/** 配对新主机（配对页「新主机配对」、串口 pairing start）：断开当前主机后发
 *  标准发现广播（Pro 单身份），等新主机搜索配对；流程一直
 *  挂着，直到新主机配上或用户停止广播。 */
void ns2_session_start_pairing_mode(void);

/** 手动配对模式是否开启（供控制面推导 UI 六态）。 */
bool ns2_session_pairing_mode_active(void);

/** 连接键（屏幕「连接」、PWR 长按 3 秒）：打开连接窗口广播等主机连上来——
 *  已配对身份发回连形态（醒着的主机看到就会连回来，因此主机停在握把/顺序页
 *  时可以先把设备留给它再按，同主机按新玩家序号重新分配）；未配对身份进
 *  配对流程发发现广播。已连接时忽略，配对流程进行时不重复开窗。 */
void ns2_session_connect(void);

/** 停止广播（屏幕「停止」/「断开」、串口 drop）：关闭连接窗口与配对流程，
 *  已连接就断开当前主机。设备回到静默，不再由本机主动发信号。 */
void ns2_session_disconnect(void);

/** 连接窗口是否在开（正在广播等主机连上来），供控制面推导 UI 六态。 */
bool ns2_session_advertising(void);

/** 唤醒请求（调试页 HOME 在未连接时、串口 wake）：打开唤醒窗口发唤醒形态
 *  0x81 把休眠中的主机叫起来；窗口到期即静默。已连接时先断开，让主机按
 *  唤醒广播重连（握把/顺序页连上来的会话不采用输入报文）。配对流程进行时
 *  忽略。 */
void ns2_session_wake_request(void);

/** 广播窗口内形态的来源（对账开关）。 */
typedef enum {
    NS2_WINDOW_FORM_AUTO = 0,      /**< 按窗口来源：连接键回连形态、HOME 唤醒形态。 */
    NS2_WINDOW_FORM_WAKE = 1,      /**< 钉住唤醒形态 0x81。 */
    NS2_WINDOW_FORM_RECONNECT = 2, /**< 钉住回连形态 0x00。 */
} ns2_window_form_t;

/** 窗口内形态的对账开关：只有串口 `adv` 诊断命令改它。 */
void ns2_session_set_window_form(ns2_window_form_t form);
ns2_window_form_t ns2_session_window_form(void);

/** 广播地址形态的对账开关（ns2_adv_addr_form_t，串口 `advaddr`）：
 *  auto 与 public 都是公共伪装地址（主机也只接受这种），
 *  random 换成派生静态随机地址做对照。不落盘；没连接又在广播时改完立即
 *  按新形态重发，已连接的链路要断开重连才换地址。 */
bool ns2_session_set_adv_addr_form(uint8_t form);
uint8_t ns2_session_adv_addr_form(void);

/** 广播 PDU 形态的对账开关（ble_ctl_adv_pdu_form_t，串口 `advpdu`）：
 *  分辨主机按 legacy 还是扩展 PDU 过滤；同样在未连接时立即重发广播。 */
void ns2_session_set_adv_pdu_form(uint8_t form);

/** LTK 注入形态（0 = 反转后写入，1 = 原样写入）。主机连上但链路未加密时
 *  用它做现场 A/B；改动在下次连接时生效。 */
void ns2_session_set_ltk_form(uint8_t form);
uint8_t ns2_session_ltk_form(void);

/** 配对是否完成：Pro 身份有配对凭证即为真。 */
bool ns2_session_paired(void);

/** 任一活跃连接的主机是否已注册：凭证匹配回连，或本会话内完成 0x15 握手。 */
bool ns2_session_host_registered(void);

/** 是否存在已通过白名单、进入握手等待的连接（配对进行中证据）。 */
bool ns2_session_waiting_pair(void);

/** 解除配对：清除当前模式全部身份的 NVS 凭证并切回发现广播；下次配对需
 * 重走 0x15。仅供控制面显式触发，「停止配对」不经过本函数。 */
void ns2_session_unpair(void);

/** 下发四段配色（机身 / 按键 / 高光 / 握把）：重建出厂块配色；host 已同步时
 *  立即生效（断开现有链路，用户按连接键后新配色随握手生效）。 */
void ns2_session_set_colors(uint32_t body_rgb, uint32_t button_rgb, uint32_t accent_rgb,
                            uint32_t grip_rgb);

/** 上报固件版本改动后重建出厂块（0x7E40 / 0x13000 的版本字段来自工厂数据）；
 *  0x10 版本查询直接读配置，无需重建。 */
void ns2_session_refresh_fw_version(void);

/* --- 输出会话视图（供 dp 的输出通道接线；ns2_output 经 sink 间接调用）--- */

/** 活跃输出会话数（当前连接数）。 */
size_t ns2_session_output_count(void);

/** 第 index 个活跃会话的身份与报告格式。 */
bool ns2_session_output_info(size_t index, uint8_t *identity, uint8_t *report_format);

/** 向第 index 个活跃会话发送编码好的报告体（内部按其连接与订阅状态投递）。 */
void ns2_session_deliver_report(size_t index, uint8_t report_id, const uint8_t *body);

/* --- 链路状态视图（串口诊断与控制面经这些接口取数）--- */

/** 单个身份的链路状态（控制面诊断取值）。 */
typedef enum {
    NS2_LINK_IDLE = 0,    /* 无会话、也不在广播 */
    NS2_LINK_ADVERTISING, /* 无会话，广播实例在发（发现或回连） */
    NS2_LINK_WAIT_PAIR,   /* 已连接，主机握手未完成 */
    NS2_LINK_NORMAL,      /* 已连接，凭证匹配（或本会话完成握手） */
} ns2_link_state_t;

/** 单个身份的链路快照。地址为 NimBLE 存储序（显示序反转）。 */
typedef struct {
    uint8_t identity;      /* ns2_identity_t */
    uint8_t state;         /* ns2_link_state_t */
    bool connected;
    uint16_t conn_handle;
    uint8_t report_format; /* 0x05 / 0x07 / 0x08 / 0x09；未连接为 0 */
    bool notify_05;        /* 主机已订阅 0x05 输入报告通道 */
    bool notify_priv;      /* 主机已订阅专用输入通道（0x07 / 0x08 / 0x09 之一） */
    uint16_t notify_priv_handle; /* 订的那个通道句柄（0 = 未订阅） */
    bool features_enabled; /* 主机已发 0x0c/0x04 启用特性（输入被采用的门槛） */
    uint32_t reports;      /* 已投递的输入报告数（订阅后计数） */
    uint16_t conn_itvl;    /* 当前连接间隔（1.25ms 单位，4 = 5ms）；未连接为 0 */
    uint8_t creds;         /* 该身份的配对凭证条数 */
    bool advertising;      /* 该身份的广播实例在发 */
    uint8_t adv_mode;      /* ns2_adv_mode_t：在发（或按当前状态会发）的广播形态 */
    bool mac_valid;
    uint8_t mac[6];
} ns2_session_status_t;

/** 当前身份列表（只有 Pro 一个），返回写入个数。 */
size_t ns2_session_mode_identities(uint8_t out[2]);

/** 指定身份的链路快照；身份不属于当前形态时返回 false。 */
bool ns2_session_status(uint8_t identity, ns2_session_status_t *out);

/** 指定身份对外广播地址（公共伪装地址，或 advaddr random 的派生形态）；
 * host 尚未同步时地址未确定，返回 false。 */
bool ns2_session_identity_mac(uint8_t identity, uint8_t out[6]);

/** 主机下发的玩家序号灯掩码（Command 0x09，bit0-3 对应 LED1-4）：取活跃会话的
 * 并集，无连接时为 0。
 * 控制面据此在首页显示四格序号指示灯。 */
uint8_t ns2_session_player_leds(void);

#ifdef __cplusplus
}
#endif

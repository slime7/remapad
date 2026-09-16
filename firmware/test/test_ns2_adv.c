/**
 * NS2 广播载荷与广播策略（ns2_adv.c）：厂商数据里的状态位是主机唯一的唤醒
 * 判据。三种形态的字节在这里钉死——状态位或主机地址写错一位，主机就认不出
 * 这台手柄；策略部分——没有被请求连接就静默（上电与主机睡下都不发信号）、
 * 配对流程发发现广播、连接窗口内已配对发回连形态、唤醒窗口内已配对发唤醒
 * 形态、调试页 HOME 按键按主机是否在线分流、JoyCon 组合的 L+R 自动注入
 * 节奏——同样在这里定死。
 *
 * 期望值取自真机 Pro Controller 2 抓包（ndeadly/switch2_controller_research
 * 的 reconnect / wake 录制）：回连状态位 0x00，唤醒状态位 0x81，两者都
 * 携带主机地址，尾部标志 0x0F 固定在厂商数据偏移 0x12。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_adv.h"

/** 真机抓包中的主机地址（存储序，显示序为 48:F1:EB:3A:EB:81）。 */
static const uint8_t s_host_mac[6] = {0x81, 0xeb, 0x3a, 0xeb, 0xf1, 0x48};

/** 发现广播：不带主机地址、状态位 0x00。 */
static const uint8_t s_discovery[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E,
    0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** 回连广播：带主机地址、状态位 0x00（与真机回连抓包逐字节一致）。 */
static const uint8_t s_reconnect[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E,
    0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x81, 0xEB, 0x3A, 0xEB, 0xF1,
    0x48, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** 唤醒广播：带主机地址、状态位 0x81（与真机唤醒抓包逐字节一致）。 */
static const uint8_t s_wake[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E,
    0x05, 0x69, 0x20, 0x00, 0x01, 0x81, 0x81, 0xEB, 0x3A, 0xEB, 0xF1,
    0x48, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void discovery_matches_capture(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];
    ns2_adv_payload(out, 0x2069, NS2_ADV_DISCOVERY, NULL);
    CHECK_BYTES(out, s_discovery, sizeof(s_discovery));

    /* 发现形态忽略传入的主机地址：首次配对前设备不知道主机是谁。 */
    ns2_adv_payload(out, 0x2069, NS2_ADV_DISCOVERY, s_host_mac);
    CHECK_BYTES(out, s_discovery, sizeof(s_discovery));
}

static void reconnect_keeps_normal_status(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];
    ns2_adv_payload(out, 0x2069, NS2_ADV_RECONNECT, s_host_mac);
    CHECK_EQ(out[5 + NS2_ADV_MFR_STATUS_OFFSET], NS2_ADV_STATUS_NORMAL);
    CHECK_BYTES(out, s_reconnect, sizeof(s_reconnect));
}

static void wake_sets_wake_status(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];
    ns2_adv_payload(out, 0x2069, NS2_ADV_WAKE, s_host_mac);
    CHECK_EQ(out[5 + NS2_ADV_MFR_STATUS_OFFSET], NS2_ADV_STATUS_WAKE);
    CHECK_BYTES(out, s_wake, sizeof(s_wake));
}

static void missing_host_mac_degrades_to_discovery(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];
    /* 未配对的设备没有主机地址：回连/唤醒形态退化为发现形态，绝不带 0x81。 */
    ns2_adv_payload(out, 0x2069, NS2_ADV_RECONNECT, NULL);
    CHECK_BYTES(out, s_discovery, sizeof(s_discovery));
    ns2_adv_payload(out, 0x2069, NS2_ADV_WAKE, NULL);
    CHECK_BYTES(out, s_discovery, sizeof(s_discovery));
    /* 静默不是形态：真被调用时按发现广播成型，绝不带主机地址或 0x81。 */
    ns2_adv_payload(out, 0x2069, NS2_ADV_OFF, s_host_mac);
    CHECK_BYTES(out, s_discovery, sizeof(s_discovery));
}

static void pid_follows_identity(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];

    /* JoyCon 2 左右两只是主机眼中的两台设备：PID 必须各不相同。 */
    ns2_adv_payload(out, 0x2067, NS2_ADV_DISCOVERY, NULL);
    CHECK_EQ(out[12], 0x67);
    CHECK_EQ(out[13], 0x20);
    ns2_adv_payload(out, 0x2066, NS2_ADV_DISCOVERY, NULL);
    CHECK_EQ(out[12], 0x66);
    CHECK_EQ(out[13], 0x20);
}

/** 广播形态决策：设备与真机一样只在被请求后广播——没有窗口就是静默，
 *  上电与主机睡下都回到这一态。 */
static void mode_choice_follows_window(void)
{
    ns2_adv_window_t win = {0};
    const int64_t t0 = 1000000;

    /* 没有窗口：静默，已配对也不例外。 */
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_OFF);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_OFF);
    CHECK_EQ(ns2_adv_choose_mode(true, false, NULL, t0), NS2_ADV_OFF);

    /* 配对流程优先：要配的是新主机，发现广播不带旧主机的地址。 */
    ns2_adv_window_open(&win, NS2_ADV_REQ_CONNECT, t0);
    CHECK_EQ(ns2_adv_choose_mode(true, true, &win, t0), NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(false, true, &win, t0), NS2_ADV_DISCOVERY);

    /* 连接窗口：已配对发回连形态（醒着的主机自己会连回来），未配对发发现
     * 广播等主机来搜。 */
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_DISCOVERY);

    /* 唤醒窗口：已配对发唤醒形态把休眠主机叫起来；未配对没有主机可唤醒，
     * 退化为发现广播。 */
    ns2_adv_window_open(&win, NS2_ADV_REQ_WAKE, t0);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_WAKE);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_DISCOVERY);

    /* 窗口到期即静默。 */
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_WAKE_WINDOW_US),
             NS2_ADV_OFF);
}

/** JoyCon 组合确认：两只都就绪（收到 0x0c/0x04、输入被采用）后立即注入
 *  L+R，拿齐凭证之前每 3 秒重试；已配对或未就绪时不注入。 */
static void lr_injection_follows_readiness(void)
{
    ns2_adv_lr_timer_t timer = {0};
    const int64_t t0 = 1000000;

    /* 就绪即注入，间隔内不重复。 */
    CHECK(ns2_adv_lr_step(&timer, false, true, t0));
    CHECK(!ns2_adv_lr_step(&timer, false, true, t0));
    CHECK(!ns2_adv_lr_step(&timer, false, true, t0 + NS2_ADV_LR_RETRY_US - 1));
    CHECK(ns2_adv_lr_step(&timer, false, true, t0 + NS2_ADV_LR_RETRY_US));

    /* 已配对之后不再注入：真机此时不需要 L+R 组合确认。 */
    CHECK(!ns2_adv_lr_step(&timer, true, true, t0 + NS2_ADV_LR_RETRY_US * 2));

    /* 只有一只在线（或未启用特性）时不注入，也不消耗计时。 */
    CHECK(!ns2_adv_lr_step(&timer, false, false, t0 + NS2_ADV_LR_RETRY_US * 3));
    CHECK(ns2_adv_lr_step(&timer, false, true, t0 + NS2_ADV_LR_RETRY_US * 3));

    /* 切换身份 / 重进配对流程后复位：下一次就绪立刻注入。 */
    ns2_adv_lr_reset(&timer);
    CHECK(ns2_adv_lr_step(&timer, false, true, t0 + NS2_ADV_LR_RETRY_US * 4));
}

static void manufacturer_data_offsets(void)
{
    uint8_t out[NS2_ADV_PAYLOAD_LEN];
    ns2_adv_payload(out, 0x2069, NS2_ADV_RECONNECT, s_host_mac);

    /* 厂商数据头两字节是 Company ID（0x0553 小端），尾部标志 0x0F 落在
     * 厂商数据偏移 0x12（载荷内 23）：状态位与地址字段写错一位都会让主机
     * 认不出这台手柄。 */
    CHECK_EQ(out[5 + 0x00], 0x53);
    CHECK_EQ(out[5 + 0x01], 0x05);
    CHECK_EQ(out[5 + 0x12], 0x0F);
    CHECK_BYTES(&out[5 + NS2_ADV_MFR_HOST_MAC_OFFSET], s_host_mac, sizeof(s_host_mac));
}

/** 休眠链路判据：已订阅但主机始终没发 0x0c/0x04 启用特性——输入被采用的
 *  门槛是特性启用而不是连接间隔（实测 itvl=4 但未启用的链路按键无效）。 */
static void dormant_link_follows_feature_enable(void)
{
    CHECK(ns2_adv_dormant_link(true, false));
    /* 主机发了 0x0c/0x04：链路可用（无论间隔多少），不能被判休眠。 */
    CHECK(!ns2_adv_dormant_link(true, true));
    /* 未订阅（初始化未走完）不算休眠。 */
    CHECK(!ns2_adv_dormant_link(false, false));
}

/** 回连广播要带回的地址是主机真正在用的那一条：配对交换给的是两条只差一位的
 *  主机地址（本设备实测末字节 0x8c / 0x8d），凭证里存的那条未必是主机连接时
 *  在用的那条——塞错一条主机就既不回连也不醒。 */
static void host_mac_prefers_last_connected_address(void)
{
    const uint8_t recorded[6] = {0x8d, 0x63, 0x27, 0x70, 0x68, 0xb8};
    const uint8_t paired[6] = {0x8c, 0x63, 0x27, 0x70, 0x68, 0xb8};
    const uint8_t older[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t zero[6] = {0};
    const uint8_t *creds[2] = {paired, older};

    CHECK(ns2_adv_choose_host_mac(recorded, creds, 2) == recorded);

    /* 记录值缺失或全零：退回最新一条可用凭证。 */
    CHECK(ns2_adv_choose_host_mac(NULL, creds, 2) == paired);
    CHECK(ns2_adv_choose_host_mac(zero, creds, 2) == paired);
    const uint8_t *with_zero[3] = {zero, paired, older};
    CHECK(ns2_adv_choose_host_mac(NULL, with_zero, 3) == paired);

    /* 一条可用地址都没有：调用方按 NULL 退化为发现广播。 */
    CHECK(ns2_adv_choose_host_mac(zero, with_zero, 1) == NULL);
    CHECK(ns2_adv_choose_host_mac(NULL, NULL, 0) == NULL);
}

/** 广播窗口的有效期：连接键与唤醒键各自取自己的时长，末微秒仍算窗口内，
 *  到期与「主机连上就收窗」都立刻失效——主机随后睡下时不能再被叫醒。 */
static void window_lifetime_follows_request(void)
{
    ns2_adv_window_t win = {0};
    const int64_t t0 = 5 * 1000 * 1000LL;

    /* 收窗状态：没有窗口，也没有请求来源。 */
    CHECK(!ns2_adv_window_active(&win, t0));
    CHECK(!ns2_adv_window_active(NULL, t0));

    /* 连接键开窗：30 秒。 */
    ns2_adv_window_open(&win, NS2_ADV_REQ_CONNECT, t0);
    CHECK_EQ(win.request, NS2_ADV_REQ_CONNECT);
    CHECK(ns2_adv_window_active(&win, t0));
    CHECK(ns2_adv_window_active(&win, t0 + NS2_ADV_CONNECT_WINDOW_US - 1));
    CHECK(!ns2_adv_window_active(&win, t0 + NS2_ADV_CONNECT_WINDOW_US));

    /* HOME 开窗：10 秒，比连接窗口短。 */
    ns2_adv_window_open(&win, NS2_ADV_REQ_WAKE, t0);
    CHECK_EQ(win.request, NS2_ADV_REQ_WAKE);
    CHECK(ns2_adv_window_active(&win, t0 + NS2_ADV_WAKE_WINDOW_US - 1));
    CHECK(!ns2_adv_window_active(&win, t0 + NS2_ADV_WAKE_WINDOW_US));

    /* 再按一次连接键：按新请求重新计时（到期时刻从再按的时刻往后算 30 秒），
     * 并把窗口时长换成连接键的。 */
    const int64_t t1 = t0 + NS2_ADV_WAKE_WINDOW_US;
    ns2_adv_window_open(&win, NS2_ADV_REQ_CONNECT, t1);
    CHECK_EQ(win.request, NS2_ADV_REQ_CONNECT);
    CHECK(ns2_adv_window_active(&win, t0 + NS2_ADV_CONNECT_WINDOW_US));
    CHECK(ns2_adv_window_active(&win, t1 + NS2_ADV_WAKE_WINDOW_US));
    CHECK(!ns2_adv_window_active(&win, t1 + NS2_ADV_CONNECT_WINDOW_US));

    /* 主机连上 / 用户停止广播：收窗后立刻失效。 */
    ns2_adv_window_close(&win);
    CHECK(!ns2_adv_window_active(&win, t0 + 1));
}

/** 调试页 HOME 按键走实体手柄语义：主机在线时它就是主页键（注入按键），
 *  未连接时按键到不了主机，转成唤醒请求去打开唤醒窗口。 */
static void home_key_follows_link_state(void)
{
    CHECK_EQ(ns2_adv_home_action(true), NS2_HOME_INJECT);
    CHECK_EQ(ns2_adv_home_action(false), NS2_HOME_WAKE);
}

HOST_TEST_SUITE(suite_ns2_adv, "ns2_adv",
                {"发现广播与真机抓包一致", discovery_matches_capture},
                {"回连广播不带唤醒标志", reconnect_keeps_normal_status},
                {"唤醒广播带 0x81 状态位", wake_sets_wake_status},
                {"缺少主机地址时退化为发现形态", missing_host_mac_degrades_to_discovery},
                {"型号 ID 随身份变化", pid_follows_identity},
                {"没被请求连接就静默，窗口内才发对应形态", mode_choice_follows_window},
                {"未配对 JoyCon 就绪后注入 L+R 并重试", lr_injection_follows_readiness},
                {"休眠链路按特性启用判定", dormant_link_follows_feature_enable},
                {"回连广播用主机最近一次连接的地址", host_mac_prefers_last_connected_address},
                {"连接窗口与唤醒窗口各按自己的时长收窗", window_lifetime_follows_request},
                {"HOME 按键按主机在线与否分流", home_key_follows_link_state},
                {"厂商数据偏移与尾部标志", manufacturer_data_offsets});

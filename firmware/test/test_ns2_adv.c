/**
 * NS2 广播载荷与广播策略（ns2_adv.c）主机端用例：钉住三种形态的字节（回连状态位 0x00、
 * 唤醒 0x81、两者都带主机地址）与窗口策略（未被请求即静默、配对流程发发现广播、
 * 窗口内分时发唤醒与回连形态、HOME 按主机是否在线分流、注册证据判定）；
 * 期望值取自 Pro Controller 2 的广播样本。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_adv.h"

/** 样本里的主机地址（存储序，显示序为 48:F1:EB:3A:EB:81）。 */
static const uint8_t s_host_mac[6] = {0x81, 0xeb, 0x3a, 0xeb, 0xf1, 0x48};

/** 发现广播：不带主机地址、状态位 0x00。 */
static const uint8_t s_discovery[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E,
    0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** 回连广播：带主机地址、状态位 0x00。 */
static const uint8_t s_reconnect[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E,
    0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x81, 0xEB, 0x3A, 0xEB, 0xF1,
    0x48, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** 唤醒广播：带主机地址、状态位 0x81。 */
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

/** 广播形态决策：设备与手柄一样只在被请求后广播——没有窗口就是静默，
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
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_SEARCH, t0);
    CHECK_EQ(ns2_adv_choose_mode(true, true, &win, t0), NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(false, true, &win, t0), NS2_ADV_DISCOVERY);

    /* 连接窗口（信号搜索）：已配对在 0~3 秒发唤醒形态（叫醒休眠主机），3 秒后发回连形态（等主机连回）；
     * 未配对发发现广播等主机来搜。 */
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_WAKE);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_WAKE_BURST_US - 1),
             NS2_ADV_WAKE);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_WAKE_BURST_US),
             NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0 + NS2_ADV_WAKE_BURST_US),
             NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_CONNECT_WINDOW_US),
             NS2_ADV_OFF);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0 + NS2_ADV_CONNECT_WINDOW_US),
             NS2_ADV_OFF);

    /* 唤醒窗口（全程唤醒突发）：已配对发唤醒形态把休眠主机叫起来；未配对
     * 没有主机可唤醒，退化为发现广播。 */
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_WAKE, t0);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_WAKE);
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_DISCOVERY);

    /* 窗口到期即静默。 */
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_WAKE_WINDOW_US),
             NS2_ADV_OFF);
}

/** 断连回连信号（组装参数不带唤醒突发）：窗口内全程回连形态，绝不发 0x81
 *  ——用户主动休眠主机后链路断开，设备自动回连不能把它立刻叫醒。 */
static void reconnect_signal_never_wakes(void)
{
    ns2_adv_window_t win = {0};
    const int64_t t0 = 2 * 1000 * 1000LL;
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_RECONNECT, t0);

    CHECK_EQ(win.burst_us, 0);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0), NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_WAKE_BURST_US - 1),
             NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_CONNECT_WINDOW_US / 2),
             NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_CONNECT_WINDOW_US - 1),
             NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(true, false, &win, t0 + NS2_ADV_CONNECT_WINDOW_US),
             NS2_ADV_OFF);

    /* 未配对身份没有主机可回连，窗口内照旧退化为发现广播等搜索。 */
    CHECK_EQ(ns2_adv_choose_mode(false, false, &win, t0), NS2_ADV_DISCOVERY);
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
 *  门槛是特性启用而不是连接间隔（itvl=4 但未启用的链路按键无效）。 */
static void dormant_link_follows_feature_enable(void)
{
    CHECK(ns2_adv_dormant_link(true, false));
    /* 主机发了 0x0c/0x04：链路可用（无论间隔多少），不能被判休眠。 */
    CHECK(!ns2_adv_dormant_link(true, true));
    /* 未订阅（初始化未走完）不算休眠。 */
    CHECK(!ns2_adv_dormant_link(false, false));
}

/** 主机注册证据：地址命中凭证、私有配对握手走完、主机在链路上启用特性
 *  （0x0c/0x04）三条任一条成立即算注册。第三条覆盖两个现场——主机换了
 *  随机地址、主机已存有本机凭证而不再重跑 0x15：只看地址会把在用的链路一直
 *  留在等待态，屏幕停在「配对中…」，配新主机的流程也退不出来。
 *  「已订阅但没启用特性」（握把页快捷回连）不算注册：主机此刻还没认这只手柄。 */
static void host_registration_follows_evidence(void)
{
    CHECK(ns2_adv_host_registered(true, false, false));
    CHECK(ns2_adv_host_registered(false, true, false));
    CHECK(ns2_adv_host_registered(false, false, true));
    CHECK(!ns2_adv_host_registered(false, false, false));
}

/** 回连广播要带回的地址是主机真正在用的那一条：配对交换给的是两条只差一位的
 *  主机地址（末字节 0x8c / 0x8d），凭证里存的那条未必是主机连接时在用的那条
 *  ——塞错一条主机就既不回连也不醒。 */
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

/** 广播窗口的有效期与分时参数都来自组装信号：各标准信号各取自己的时长，
 *  末微秒仍算窗口内，到期与「主机连上就收窗」都立刻失效——主机随后睡下时
 *  不能再被叫醒。 */
static void window_lifetime_follows_signal(void)
{
    ns2_adv_window_t win = {0};
    const int64_t t0 = 5 * 1000 * 1000LL;

    /* 收窗状态：没有窗口。 */
    CHECK(!ns2_adv_window_active(&win, t0));
    CHECK(!ns2_adv_window_active(NULL, t0));

    /* 连接键信号搜索：30 秒，前 3 秒唤醒突发。 */
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_SEARCH, t0);
    CHECK_EQ(win.until_us - win.opened_at_us, NS2_ADV_CONNECT_WINDOW_US);
    CHECK_EQ(win.burst_us, NS2_ADV_WAKE_BURST_US);
    CHECK(ns2_adv_window_active(&win, t0));
    CHECK(ns2_adv_window_active(&win, t0 + NS2_ADV_CONNECT_WINDOW_US - 1));
    CHECK(!ns2_adv_window_active(&win, t0 + NS2_ADV_CONNECT_WINDOW_US));

    /* HOME 唤醒：10 秒全程唤醒突发，比连接窗口短。 */
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_WAKE, t0);
    CHECK_EQ(win.until_us - win.opened_at_us, NS2_ADV_WAKE_WINDOW_US);
    CHECK_EQ(win.burst_us, NS2_ADV_WAKE_WINDOW_US);
    CHECK(ns2_adv_window_active(&win, t0 + NS2_ADV_WAKE_WINDOW_US - 1));
    CHECK(!ns2_adv_window_active(&win, t0 + NS2_ADV_WAKE_WINDOW_US));

    /* 断连回连：同为 30 秒但不带唤醒突发；重新开窗按新信号换算并重新计时
     * （到期时刻从再开的时刻往后算 30 秒）。 */
    const int64_t t1 = t0 + NS2_ADV_WAKE_WINDOW_US;
    ns2_adv_window_open(&win, &NS2_ADV_SIGNAL_RECONNECT, t1);
    CHECK_EQ(win.until_us - win.opened_at_us, NS2_ADV_CONNECT_WINDOW_US);
    CHECK_EQ(win.burst_us, 0);
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

/** HOME 的按下沿：按住 HOME 只叫醒一次（一直按住不重复开窗），松开再按
 *  才算下一次按下。 */
static void home_key_fires_on_press_edge(void)
{
    ns2_adv_home_key_t key = {0};

    CHECK(!ns2_adv_home_key_step(&key, false));
    CHECK(ns2_adv_home_key_step(&key, true));
    CHECK(!ns2_adv_home_key_step(&key, true));
    CHECK(!ns2_adv_home_key_step(&key, false));
    CHECK(ns2_adv_home_key_step(&key, true));
}

HOST_TEST_SUITE(suite_ns2_adv, "ns2_adv",
                {"发现广播与样本一致", discovery_matches_capture},
                {"回连广播不带唤醒标志", reconnect_keeps_normal_status},
                {"唤醒广播带 0x81 状态位", wake_sets_wake_status},
                {"缺少主机地址时退化为发现形态", missing_host_mac_degrades_to_discovery},
                {"型号 ID 随身份变化", pid_follows_identity},
                {"没被请求连接就静默，窗口内才发对应形态", mode_choice_follows_window},
                {"休眠链路按特性启用判定", dormant_link_follows_feature_enable},
                {"主机注册按地址、配对握手或特性启用判定", host_registration_follows_evidence},
                {"断连回连窗口全程回连形态不发唤醒", reconnect_signal_never_wakes},
                {"回连广播用主机最近一次连接的地址", host_mac_prefers_last_connected_address},
                {"窗口时长与唤醒突发随组装信号而定", window_lifetime_follows_signal},
                {"HOME 按键按主机在线与否分流", home_key_follows_link_state},
                {"按住 HOME 只触发一次唤醒", home_key_fires_on_press_edge},
                {"厂商数据偏移与尾部标志", manufacturer_data_offsets});

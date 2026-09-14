/**
 * NS2 广播载荷（ns2_adv.c）：厂商数据里的状态位是主机唯一的唤醒判据。
 * 回连形态若带了唤醒标志，休眠中的主机会被每一次回连广播立刻唤醒——这
 * 正是「NS2 自动待机后马上亮起」的成因，因此三种形态的字节在这里钉死。
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

/** 唤醒窗口有界：开机与「唤醒 HOME」靠它叫醒主机，窗口一旦没关上，休眠中
 *  的主机会被每一次广播反复叫醒（用户可见的「一待机就亮屏」）。 */
static void wake_window_is_bounded(void)
{
    ns2_adv_wake_window_t win = {0};
    CHECK(!ns2_adv_wake_window_active(&win, 0));
    CHECK(!ns2_adv_wake_window_active(&win, 1234567));

    ns2_adv_wake_window_open(&win, 1000, 2000000);
    CHECK(ns2_adv_wake_window_active(&win, 1000));
    CHECK(ns2_adv_wake_window_active(&win, 1000 + 1999999));
    CHECK(!ns2_adv_wake_window_active(&win, 1000 + 2000000));

    /* 再次请求是顺延：连续按键唤醒不该缩短已经开启的窗口。 */
    ns2_adv_wake_window_open(&win, 1500000, 2000000);
    CHECK(ns2_adv_wake_window_active(&win, 3000000));
    CHECK(!ns2_adv_wake_window_active(&win, 3500000));

    /* 主机连上后立即关窗：后继广播回到 0x00 的回连形态。 */
    ns2_adv_wake_window_close(&win);
    CHECK(!ns2_adv_wake_window_active(&win, 1500001));
}

/** 广播形态决策：未配对身份绝不允许发唤醒广播（不能把主机从休眠里叫醒）。 */
static void mode_choice_follows_pairing(void)
{
    CHECK_EQ(ns2_adv_choose_mode(false, false), NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(false, true), NS2_ADV_DISCOVERY);
    CHECK_EQ(ns2_adv_choose_mode(true, false), NS2_ADV_RECONNECT);
    CHECK_EQ(ns2_adv_choose_mode(true, true), NS2_ADV_WAKE);
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

HOST_TEST_SUITE(suite_ns2_adv, "ns2_adv",
                {"发现广播与真机抓包一致", discovery_matches_capture},
                {"回连广播不带唤醒标志", reconnect_keeps_normal_status},
                {"唤醒广播带 0x81 状态位", wake_sets_wake_status},
                {"缺少主机地址时退化为发现形态", missing_host_mac_degrades_to_discovery},
                {"型号 ID 随身份变化", pid_follows_identity},
                {"唤醒窗口有界且连接后立即关上", wake_window_is_bounded},
                {"未配对身份不发唤醒广播", mode_choice_follows_pairing},
                {"休眠链路按特性启用判定", dormant_link_follows_feature_enable},
                {"厂商数据偏移与尾部标志", manufacturer_data_offsets});

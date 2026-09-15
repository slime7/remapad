/**
 * Nintendo 家族布局（pad/layouts/ns.c）：Switch 一代的 0x30 / 0x3F 与 Switch 2
 * 的 0x05 / 0x09 报文体。这里钉住三件事——按键位置语义、12 位打包摇杆与多
 * 样本运动数据取最新一份，以及同代透传所需的载荷（raw）与语言标记。
 *
 * 偏移按公开资料登记、本轮没有实机核对，样本按同一份资料构造；实机用
 * pc/bridge.py --dump 抓包对账（ROADMAP M5 的实机待办）。
 */
#include "host_test.h"

#include <string.h>

#include "pad_device.h"
#include "pad_state.h"

/** Switch 2 的 0x09 报文体：首字节是 Report ID，报文体偏移整体加 1。 */
static pad_report_t ns2_09_report(uint16_t pid)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_UNKNOWN;
    report.conn = PAD_CONN_USB;
    report.vid = 0x057E;
    report.pid = pid;
    report.report_id = 0x09;
    report.len = 64;
    report.data[0] = 0x09;
    /* 电源字节：bit1 充电中、bits2-5 电量等级 9（满）。 */
    report.data[2] = 0x26;
    return report;
}

/** Switch 一代的 0x30 报文体（Pro Controller）。 */
static pad_report_t ns1_30_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_UNKNOWN;
    report.conn = PAD_CONN_USB;
    report.vid = 0x057E;
    report.pid = 0x2009;
    report.report_id = 0x30;
    report.len = 49;
    report.data[0] = 0x30;
    /* 摇杆中位（单字节 0x80）。 */
    report.data[6] = 0x80;
    report.data[7] = 0x80;
    report.data[8] = 0x80;
    report.data[9] = 0x80;
    return report;
}

/** 把 X / Y 打包成 12 位紧凑三字节（与 ns2_pack_stick 同一套规则）。 */
static void pack_u12(uint16_t x, uint16_t y, uint8_t *out)
{
    out[0] = (uint8_t)(x & 0xFF);
    out[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    out[2] = (uint8_t)((y >> 4) & 0xFF);
}

static void ns2_pad_rows_resolve_by_pid(void)
{
    /* Pro Controller 2：家族按 VID 判定、行按 PID 命中，能力位不标兜底。 */
    pad_report_t report = ns2_09_report(0x2069);
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_NS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);
    CHECK_EQ(state.native_lang, PAD_LANG_NS2);
    CHECK_EQ(state.native_identity, PAD_IDENTITY_PRO);
    CHECK_EQ(state.caps & PAD_CAP_BATTERY, PAD_CAP_BATTERY);

    /* Joy-Con 2 左右各自命中一行，期望身份不同。 */
    report = ns2_09_report(0x2067);
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.native_identity, PAD_IDENTITY_JOYCON_L);
    report = ns2_09_report(0x2066);
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.native_identity, PAD_IDENTITY_JOYCON_R);
}

static void ns2_09_body_parses_buttons_sticks_and_battery(void)
{
    pad_report_t report = ns2_09_report(0x2069);
    /* b0：B（下）与 A（右）；b1：方向键下；b2：C 键与 GR。 */
    report.data[3] = 0x03;
    report.data[4] = 0x01;
    report.data[5] = 0x14;
    /* 左摇杆推满右、右摇杆推满上（原始 Y 越小越靠上）。 */
    pack_u12(4095, 2048, &report.data[6]);
    pack_u12(2048, 0, &report.data[9]);
    pad_state_t state;
    pad_state_from_report(&report, &state);

    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_CIRCLE | PAD_BTN_DPAD_DOWN | PAD_BTN_MUTE |
                        PAD_BTN_R4));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MAX);
    CHECK_EQ(state.axis[PAD_AXIS_RX], PAD_AXIS_CENTER);
    CHECK(state.axis[PAD_AXIS_RY] > PAD_AXIS_CENTER); /* 设备上推 → 私有向上为正 */
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_CENTER);
    /* 电量等级 9 → 100%，充电位来自 bit1。 */
    CHECK_EQ(state.battery_percent, 100);
    CHECK(state.charging);
}

static void ns2_report_is_kept_verbatim_for_relay(void)
{
    pad_report_t report = ns2_09_report(0x2069);
    report.data[0x0F] = 0xAA; /* 运动块首字节：透传时原样带走 */
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.raw_len, report.len);
    CHECK_EQ(state.raw_report_id, 0x09);
    CHECK_BYTES(state.raw, report.data, report.len);

    /* 报告标识与报文首字节不符时不做透传（元数据与载荷对不上）。 */
    report.report_id = 0x05;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.raw_len, 0);
}

static void ns1_pro_report_maps_buttons_sticks_and_motion(void)
{
    pad_report_t report = ns1_30_report();
    /* b0：Y（左）→ □、A（右）→ ○、R → R1；b1：Plus → 选项、Home → 主页；
     * b2：Down 与 ZL（ZL 是数字位，私有格式不映射）。 */
    report.data[3] = 0x49;
    report.data[4] = 0x12;
    report.data[5] = 0x81;
    /* 左摇杆推满下：原始 Y 增大 → 私有向上为负。 */
    report.data[7] = 0xFF;
    /* 运动：三份样本，只有最后一份有效（最新采样）。 */
    report.data[13 + 12 * 0] = 0x11;
    report.data[13 + 12 * 2] = 0x2C;
    report.data[14 + 12 * 2] = 0x01; /* 300 */
    report.data[13 + 12 * 2 + 6] = 0xFF;
    report.data[14 + 12 * 2 + 6] = 0xFF; /* 加速 X = -1 */
    pad_state_t state;
    pad_state_from_report(&report, &state);

    CHECK_EQ(state.family, PAD_FAMILY_NS);
    CHECK_EQ(state.native_lang, PAD_LANG_NS1);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_SQUARE | PAD_BTN_CIRCLE | PAD_BTN_R1 | PAD_BTN_OPT |
                        PAD_BTN_HOME | PAD_BTN_DPAD_DOWN));
    CHECK(state.axis[PAD_AXIS_LY] < PAD_AXIS_CENTER);
    CHECK_EQ(state.caps & PAD_CAP_MOTION, PAD_CAP_MOTION);
    CHECK(state.motion.present);
    CHECK_EQ(state.motion.gyro[0], 300);
    CHECK_EQ(state.motion.accel[0], -1);
    CHECK_EQ(state.motion.gyro[1], 0);
}

static void unknown_ns_pid_falls_back(void)
{
    /* 未登记的 Nintendo 设备：家族认得出来，但没有布局行 → 兜底并标记。 */
    pad_report_t report = ns2_09_report(0x1234);
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_NS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, PAD_CAP_FALLBACK_LAYOUT);
    CHECK_EQ(state.native_lang, PAD_LANG_NONE);
    CHECK_EQ(state.raw_len, 0);
}

HOST_TEST_SUITE(suite_pad_ns, "pad_ns",
                {"NS2 报文体按 PID 命中布局行并带上透传语言",
                 ns2_pad_rows_resolve_by_pid},
                {"NS2 的 0x09 报文体解析按键、打包摇杆与电量",
                 ns2_09_body_parses_buttons_sticks_and_battery},
                {"NS2 报文体原样留作透传载荷", ns2_report_is_kept_verbatim_for_relay},
                {"NS1 的 0x30 报文体解析按键、摇杆与三份运动样本取最新",
                 ns1_pro_report_maps_buttons_sticks_and_motion},
                {"未登记的 NS 型号回落兜底布局", unknown_ns_pid_falls_back});

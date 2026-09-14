/**
 * 转换段（target/ns2）：私有格式到 NS2 报文的映射错了，真机上表现为
 * 「按 A 出了 B」「扳机没反应」或「背键丢失」，这几条正是桥接验收要看的
 * 现象。这里把会话通道换成捕获回调，驱动真实的 ns2_output 编码后断言报文字节，
 * 因此面键位置、背键折并、扳机阈值与电量折叠都在真实编码路径上验证。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_output.h"
#include "ns2_report.h"
#include "ns2_state.h"
#include "ns2_target.h"
#include "pad_state.h"
#include "target.h"

/** 捕获一次发送：0x09 报文体长度固定，直接按字段偏移回读。 */
static struct {
    uint8_t report_id;
    uint8_t body[NS2_INPUT_09_LEN];
    size_t len;
    unsigned sends;
} s_capture;

static size_t capture_session_count(void *user)
{
    (void)user;
    return 1;
}

static bool capture_session_info(size_t index, uint8_t *identity, uint8_t *report_format,
                                 void *user)
{
    (void)user;
    if (index != 0) {
        return false;
    }
    *identity = NS2_ID_PRO;
    *report_format = NS2_REPORT_ID_09;
    return true;
}

static void capture_send_report(size_t index, uint8_t report_id, const uint8_t *body, size_t len,
                                void *user)
{
    (void)index;
    (void)user;
    s_capture.report_id = report_id;
    s_capture.len = len < sizeof(s_capture.body) ? len : sizeof(s_capture.body);
    memcpy(s_capture.body, body, s_capture.len);
    s_capture.sends++;
}

/** 每个用例开头调用：装好 NS2 目标与捕获通道，并把上一轮的报文清掉。 */
static void prepare(void)
{
    static const ns2_output_sink_t sink = {
        .session_count = capture_session_count,
        .session_info = capture_session_info,
        .send_report = capture_send_report,
        .user = NULL,
    };
    static const pad_target_facts_t facts = {
        .battery_level = 0,
        .battery_mv = 0,
        .charging = false,
        .external_power = false,
        .rumble_enabled = false,
        .nfc_state = 0,
    };
    ns2_output_set_sink(&sink);
    target_set(ns2_target_get());
    target_set_facts(&facts);
    memset(&s_capture, 0, sizeof(s_capture));
}

/** 只按住一个私有按键：断言 NS2 报文里除该位以外没有别的按键。 */
static void expect_buttons(uint32_t pad_buttons, uint8_t offset, uint8_t bit)
{
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.buttons = pad_buttons;
    const unsigned before = s_capture.sends;
    target_send_pad(&pad);
    /* 一次采样只发一轮报告：多发或少发都说明映射层改变了吞吐。 */
    CHECK_EQ(s_capture.sends, before + 1);
    for (uint8_t i = 0x02; i <= 0x04; i++) {
        const uint8_t want = i == offset ? (uint8_t)(1u << bit) : 0x00;
        CHECK_EQ(s_capture.body[i], want);
    }
}

static void face_buttons_keep_position_semantics(void)
{
    prepare();
    /* 私有用 PS 键名、NS2 用 Nintendo 标签，按位置一一对应：○ 右 → A，其余同。 */
    expect_buttons(PAD_BTN_CIRCLE, 0x02, 1);
    expect_buttons(PAD_BTN_CROSS, 0x02, 0);
    expect_buttons(PAD_BTN_TRIANGLE, 0x02, 3);
    expect_buttons(PAD_BTN_SQUARE, 0x02, 2);
}

static void shoulders_dpad_and_system_keys(void)
{
    prepare();
    expect_buttons(PAD_BTN_LB, 0x03, 4);
    expect_buttons(PAD_BTN_RB, 0x02, 4);
    expect_buttons(PAD_BTN_DPAD_UP, 0x03, 3);
    expect_buttons(PAD_BTN_DPAD_DOWN, 0x03, 0);
    expect_buttons(PAD_BTN_DPAD_LEFT, 0x03, 2);
    expect_buttons(PAD_BTN_DPAD_RIGHT, 0x03, 1);
    expect_buttons(PAD_BTN_START, 0x02, 6);
    expect_buttons(PAD_BTN_BACK, 0x03, 6);
    expect_buttons(PAD_BTN_GUIDE, 0x04, 0);
    expect_buttons(PAD_BTN_SHARE, 0x04, 1);
    expect_buttons(PAD_BTN_LSTICK, 0x03, 7);
    expect_buttons(PAD_BTN_RSTICK, 0x02, 7);
    /* 目标专属键：主机上没有对应键，只有目标侧会填。 */
    expect_buttons(PAD_BTN_C, 0x04, 4);
}

static void back_buttons_fold_into_gl_and_gr(void)
{
    prepare();
    /* NS2 只有 GL / GR 两个扩展键，四颗背键按侧合并。 */
    expect_buttons(PAD_BTN_L4, 0x04, 3);
    expect_buttons(PAD_BTN_L5, 0x04, 3);
    expect_buttons(PAD_BTN_R4, 0x04, 2);
    expect_buttons(PAD_BTN_R5, 0x04, 2);
}

static void analog_triggers_digitize_at_half(void)
{
    pad_state_t pad;
    pad_state_defaults(&pad);

    /* 阈值下侧：两位都不亮。 */
    pad.trigger[PAD_TRIGGER_L] = 2047;
    pad.trigger[PAD_TRIGGER_R] = 2047;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x02] & 0x20, 0);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0);

    /* 阈值上侧：ZL 与 ZR 同时点亮。 */
    pad.trigger[PAD_TRIGGER_L] = 2048;
    pad.trigger[PAD_TRIGGER_R] = 2048;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x02] & 0x20, 0x20);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0x20);

    /* 全按与刚过阈值在报文里没有区别（NS2 只有数字扳机）。 */
    pad.trigger[PAD_TRIGGER_L] = PAD_AXIS_MAX;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0x20);

    /* 松开：回到不亮。 */
    pad.trigger[PAD_TRIGGER_L] = 0;
    pad.trigger[PAD_TRIGGER_R] = 0;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x02] & 0x20, 0);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0);
}

static void sticks_keep_values_and_center(void)
{
    prepare();
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.axis[PAD_AXIS_LX] = 0x123;
    pad.axis[PAD_AXIS_LY] = 0xFFF;
    pad.axis[PAD_AXIS_RX] = PAD_AXIS_MIN;
    pad.axis[PAD_AXIS_RY] = PAD_AXIS_CENTER;
    target_send_pad(&pad);

    uint16_t lx = 0;
    uint16_t ly = 0;
    uint16_t rx = 0;
    uint16_t ry = 0;
    ns2_unpack_stick(&s_capture.body[0x05], &lx, &ly);
    ns2_unpack_stick(&s_capture.body[0x08], &rx, &ry);
    CHECK_EQ(lx, 0x123);
    CHECK_EQ(ly, 0xFFF);
    CHECK_EQ(rx, PAD_AXIS_MIN);
    CHECK_EQ(ry, PAD_AXIS_CENTER);
}

static void target_facts_fold_into_power_byte(void)
{
    prepare();
    const pad_target_facts_t facts = {
        .battery_level = 5,
        .battery_mv = 3800,
        .charging = true,
        .external_power = true,
    };
    target_set_facts(&facts);

    pad_state_t pad;
    pad_state_defaults(&pad);
    target_send_pad(&pad);
    /* 0x09 电源字节：电量占高 6 位所在区段，充电与外部供电各占一位。 */
    CHECK_EQ(s_capture.body[0x01], (uint8_t)((5u << 2) | 0x02u | 0x01u));
}

static void unconsumed_caps_do_not_change_the_report(void)
{
    prepare();
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.buttons = PAD_BTN_CIRCLE;
    /* 本轮 NS2 目标不吃运动、触摸板与麦克风：报文里只应体现按键。 */
    pad.caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_MIC;
    pad.motion.present = true;
    pad.motion.gyro[0] = 1234;
    pad.touch[PAD_TOUCH_LEFT].present = true;
    pad.touch[PAD_TOUCH_LEFT].pressed = true;
    pad.mic_level = 4095;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.sends, 1);
    CHECK_EQ(s_capture.body[0x02], 0x02);
    CHECK_EQ(s_capture.body[0x03], 0x00);
    CHECK_EQ(s_capture.body[0x04], 0x00);
}

static void unknown_model_still_reports_keys(void)
{
    prepare();
    /* 未识别型号走 Xbox 兜底：标了能力位也要照常出报文，主机侧不能没反应。 */
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.buttons = PAD_BTN_CROSS;
    pad.caps = PAD_CAP_FALLBACK_LAYOUT;
    pad.family = PAD_FAMILY_UNKNOWN;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.sends, 1);
    CHECK_EQ(s_capture.body[0x02], 0x01);
}

HOST_TEST_SUITE(suite_target_ns2, "target_ns2",
                {"面键按位置映射到 NS2 的 A/B/X/Y（私有用 PS 键名）",
                 face_buttons_keep_position_semantics},
                {"肩键、方向键、系统键与 C 键", shoulders_dpad_and_system_keys},
                {"四颗背键按侧折进 GL / GR", back_buttons_fold_into_gl_and_gr},
                {"扳机按 50% 阈值数字化成 ZL / ZR", analog_triggers_digitize_at_half},
                {"摇杆原样进报文且中位正确", sticks_keep_values_and_center},
                {"目标事实折进电量字节", target_facts_fold_into_power_byte},
                {"NS2 吃不下能力位也不改报文", unconsumed_caps_do_not_change_the_report},
                {"未识别型号兜底后仍照常上报", unknown_model_still_reports_keys});

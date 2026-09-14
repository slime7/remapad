/**
 * 私有格式解析（pad_device.c）：家族布局表的偏移错了会表现为「按 A 出了 B」
 * 或「摇杆漂移」，这类问题在真机上只能靠猜；这里用构造好的报告把每个家族
 * 的按键位置映射、量程归一、死区与兜底行为逐条钉住。
 *
 * 报告样本按家族表的偏移构造，与 pc/bridge.py --dump 的实测结果对账。
 */
#include "host_test.h"

#include <string.h>

#include "pad_device.h"
#include "pad_state.h"

static pad_report_t xbox_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_XBOX;
    report.conn = PAD_CONN_USB;
    report.vid = 0x045E;
    report.pid = 0x028E;
    report.report_id = 0x00;
    report.len = 20;
    report.data[1] = 0x00;
    report.data[2] = 0x00;
    /* 摇杆中位：有符号 16 位全 0。 */
    return report;
}

static void xbox_face_buttons_map_by_position(void)
{
    pad_report_t report = xbox_report();
    /* 物理 A 在下（byte1 bit4）、物理 B 在右（byte1 bit5）。 */
    report.data[2] = 0x10;
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_XBOX);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CROSS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);

    report.data[2] = 0x20;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CIRCLE);

    /* 物理 X 在左 → □ 位、物理 Y 在上 → △ 位。 */
    report.data[2] = 0x40;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SQUARE);
    report.data[2] = 0x80;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TRIANGLE);
}

static void xbox_dpad_shoulders_and_sticks(void)
{
    pad_report_t report = xbox_report();
    report.data[1] = 0x03; /* 方向键上 + 下 */
    report.data[2] = 0x03; /* LB + RB */
    /* 左摇杆推满右：+32767；右摇杆推满下：-32768（XInput 的 Y 轴正为上）。 */
    report.data[5] = 0xFF;
    report.data[6] = 0x7F;
    report.data[11] = 0x00;
    report.data[12] = 0x80;
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_DPAD_UP | PAD_BTN_DPAD_DOWN | PAD_BTN_LB | PAD_BTN_RB));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MAX);
    CHECK_EQ(state.axis[PAD_AXIS_RY], PAD_AXIS_MIN);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_CENTER);

    /* 左摇杆推满上：正满量程。 */
    report.data[7] = 0xFF;
    report.data[8] = 0x7F;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_MAX);

    /* 扳机保持模拟量：LT 全按、RT 半按。 */
    report.data[3] = 0xFF;
    report.data[4] = 0x80;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L], PAD_AXIS_MAX);
    CHECK(state.trigger[PAD_TRIGGER_R] > 2000);
    CHECK(state.trigger[PAD_TRIGGER_R] < 2100);
}

static void ps_report_parses_hat_face_buttons_and_battery(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_USB;
    report.vid = 0x054C;
    report.pid = 0x09CC;
    report.report_id = 0x01;
    report.len = 64;
    report.data[0] = 0x01;
    report.data[1] = 0x80; /* LX 中位 */
    report.data[2] = 0x80; /* LY 中位 */
    report.data[3] = 0x80; /* RX 中位 */
    report.data[4] = 0x80; /* RY 中位 */
    report.data[5] = 0x28; /* Cross（下）+ 帽子开关松开（8） */
    report.data[6] = 0x23; /* L1 + R1 + options */
    report.data[7] = 0x03; /* PS + 触摸板按下 */
    report.data[8] = 0xFF; /* L2 全按 */
    report.data[9] = 0x00; /* R2 松开 */
    report.data[12] = 0x1A; /* 电量 10 档 + 充电中 */
    report.data[34] = 0x40; /* 触摸点 X 低位 */
    report.data[35] = 0x05;
    report.data[36] = 0x02;

    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_LB | PAD_BTN_RB | PAD_BTN_START | PAD_BTN_GUIDE |
                        PAD_BTN_TOUCHPAD));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L], PAD_AXIS_MAX);
    CHECK_EQ(state.trigger[PAD_TRIGGER_R], PAD_AXIS_MIN);
    CHECK_EQ(state.battery_percent, 100);
    CHECK(state.charging);
    CHECK_EQ(state.caps & PAD_CAP_TOUCHPAD, PAD_CAP_TOUCHPAD);
    CHECK_EQ(state.caps & PAD_CAP_MOTION, PAD_CAP_MOTION);
    CHECK(state.touch[PAD_TOUCH_LEFT].present);
    CHECK(state.touch[PAD_TOUCH_LEFT].pressed);

    /* DualSense 在 PS 键与触摸板按下之外还多一个静音位（byte7 bit2）。 */
    report.data[7] = 0x07;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_GUIDE | PAD_BTN_TOUCHPAD | PAD_BTN_MUTE),
             (uint32_t)(PAD_BTN_GUIDE | PAD_BTN_TOUCHPAD | PAD_BTN_MUTE));
    report.data[7] = 0x03;

    /* 帽子开关：向上时只出方向键上。 */
    report.data[5] = 0x00;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_DPAD_UP | PAD_BTN_DPAD_DOWN | PAD_BTN_DPAD_LEFT |
                              PAD_BTN_DPAD_RIGHT),
             (uint32_t)PAD_BTN_DPAD_UP);
    CHECK_EQ(state.buttons & PAD_BTN_CROSS, 0);

    /* 斜向：右上同时置两位。 */
    report.data[5] = 0x01;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_DPAD_UP | PAD_BTN_DPAD_RIGHT),
             (uint32_t)(PAD_BTN_DPAD_UP | PAD_BTN_DPAD_RIGHT));
}

static void stick_deadzone_and_y_direction(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_USB;
    report.report_id = 0x01;
    report.len = 64;
    report.data[0] = 0x01;
    report.data[1] = 0x84; /* LX 略偏，落在死区内 */
    report.data[2] = 0x90; /* LY 明显偏下，越过死区 */
    report.data[3] = 0x00; /* RX 全左 */
    report.data[4] = 0x00; /* RY 全上（PS 报告 0 在上、255 在下） */
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK(state.axis[PAD_AXIS_LY] < PAD_AXIS_CENTER); /* 设备下推 → 私有格式向上为正 */
    CHECK_EQ(state.axis[PAD_AXIS_RX], PAD_AXIS_MIN);
    CHECK_EQ(state.axis[PAD_AXIS_RY], PAD_AXIS_MAX);
}

static void unknown_model_falls_back_to_xbox_layout(void)
{
    pad_report_t report = xbox_report();
    report.family = PAD_FAMILY_UNKNOWN;
    report.vid = 0x1234;
    report.pid = 0x5678;
    report.data[2] = 0x10;
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_UNKNOWN);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, PAD_CAP_FALLBACK_LAYOUT);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CROSS);

    /* 空报告必须安全返回，不读越界。 */
    report.len = 0;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, 0);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
}

static void family_detection_and_steam_gap(void)
{
    CHECK_EQ(pad_family_from_ids(0x045E, 0x028E), PAD_FAMILY_XBOX);
    CHECK_EQ(pad_family_from_ids(0x054C, 0x09CC), PAD_FAMILY_PS);
    CHECK_EQ(pad_family_from_ids(0x28DE, 0x1142), PAD_FAMILY_STEAM);
    CHECK_EQ(pad_family_from_ids(0x0F0D, 0x00C1), PAD_FAMILY_UNKNOWN);

    /* Steam 原生布局尚未抓包：按兜底路径解析并在能力位里如实标记。 */
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_UNKNOWN;
    report.conn = PAD_CONN_USB;
    report.vid = 0x28DE;
    report.pid = 0x1142;
    report.report_id = 0x01;
    report.len = 64;
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_STEAM);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, PAD_CAP_FALLBACK_LAYOUT);
}

HOST_TEST_SUITE(suite_pad_device, "pad_device",
                {"Xbox 面键按位置映射（物理 A 下 → ✕、物理 B 右 → ○）",
                 xbox_face_buttons_map_by_position},
                {"Xbox 方向键、肩键与摇杆量程", xbox_dpad_shoulders_and_sticks},
                {"PS 报告：帽子开关、面键、电量、触摸板与静音键",
                 ps_report_parses_hat_face_buttons_and_battery},
                {"摇杆死区与 Y 轴方向", stick_deadzone_and_y_direction},
                {"未识别型号回落 Xbox 布局并标记兜底", unknown_model_falls_back_to_xbox_layout},
                {"VID 判定家族（Steam 布局未定，走兜底）", family_detection_and_steam_gap});

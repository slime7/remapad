/**
 * 私有格式解析（pad_device.c）主机端用例：按家族表偏移构造报告，钉住各家族的按键位置映射、
 * 量程归一、死区与兜底行为；样本与 pc/remapadctl.py --dump 的结果对账。
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
             (uint32_t)(PAD_BTN_DPAD_UP | PAD_BTN_DPAD_DOWN | PAD_BTN_L1 | PAD_BTN_R1));
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
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK(state.trigger[PAD_TRIGGER_R2] > 2000);
    CHECK(state.trigger[PAD_TRIGGER_R2] < 2100);
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
    report.data[30] = 0x1A; /* 电量 10 档 + 充电中（status[0] 在 0x1E） */
    /* 触摸点：偏移 35 起每点 4 字节（触点字节 + 12 位 X + 12 位 Y），
     * X = 0x340 落在左半区。 */
    report.data[35] = 0x05; /* 触点 5：bit7 为 0 表示有触点 */
    report.data[36] = 0x40;
    report.data[37] = 0x03;
    report.data[38] = 0x02;

    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_OPT | PAD_BTN_HOME |
                        PAD_BTN_SHARE));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK_EQ(state.trigger[PAD_TRIGGER_R2], PAD_AXIS_MIN);
    CHECK_EQ(state.battery_percent, 100);
    CHECK(state.charging);
    CHECK_EQ(state.caps & PAD_CAP_TOUCHPAD, PAD_CAP_TOUCHPAD);
    CHECK_EQ(state.caps & PAD_CAP_MOTION, PAD_CAP_MOTION);
    CHECK(state.touch[PAD_TOUCH_LEFT].present);
    CHECK(state.touch[PAD_TOUCH_LEFT].pressed);
    CHECK_EQ(state.touch[PAD_TOUCH_LEFT].raw_x, 0x340);
    CHECK(state.touch[PAD_TOUCH_LEFT].x < PAD_AXIS_CENTER);
    CHECK(!state.touch[PAD_TOUCH_RIGHT].pressed);

    /* DualSense 在 PS 键与触摸板按下之外还多一个静音位（byte7 bit2）。 */
    report.data[7] = 0x07;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_HOME | PAD_BTN_SHARE | PAD_BTN_MUTE),
             (uint32_t)(PAD_BTN_HOME | PAD_BTN_SHARE | PAD_BTN_MUTE));
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

    /* Steam 原生布局尚未登记：按兜底路径解析并在能力位里如实标记。 */
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

static pad_report_t ds3_report(pad_conn_t conn)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = conn;
    report.vid = 0x054C;
    report.pid = 0x0268; /* DualShock 3 */
    report.report_id = 0x01;
    report.len = 49;
    report.data[0] = 0x01;
    report.data[1] = 0x80; /* LX / LY / RX / RY 都在中位 */
    report.data[2] = 0x80;
    report.data[3] = 0x80;
    report.data[4] = 0x80;
    return report;
}

/**
 * DualShock 3 的有线与蓝牙共用一行（偏移按 49 字节的归一化形式登记）。
 * 方向键在按键位图里，Select 与 Start 换成减号与加号的位置语义。
 */
static void dualshock3_parses_on_usb_and_bt(void)
{
    pad_report_t report = ds3_report(PAD_CONN_USB);
    report.data[5] = 0x05;  /* Select + R3 */
    report.data[6] = 0x14;  /* L1 + Triangle */
    report.data[7] = 0x01;  /* PS 键 */
    report.data[12] = 0xFF; /* L2 压力值满量程 */
    report.data[13] = 0x80; /* R2 半按 */

    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_TOUCHPAD | PAD_BTN_R3 | PAD_BTN_L1 | PAD_BTN_TRIANGLE |
                        PAD_BTN_HOME));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK(state.trigger[PAD_TRIGGER_R2] > 2000);
    CHECK(state.trigger[PAD_TRIGGER_R2] < 2100);
    /* 没有触摸板与电量字段：能力位不置位。 */
    CHECK_EQ(state.caps & PAD_CAP_TOUCHPAD, 0);
    CHECK_EQ(state.caps & PAD_CAP_BATTERY, 0);

    /* 方向键在按键位图里（bit4 上、bit5 右、bit6 下、bit7 左）。 */
    memset(report.data + 5, 0, 3);
    report.data[5] = 0x10;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_DPAD_UP);
    report.data[5] = 0x80;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_DPAD_LEFT);
    report.data[5] = 0x09; /* Select + Start */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)(PAD_BTN_TOUCHPAD | PAD_BTN_OPT));

    /* 蓝牙下同样的字节给出同样的按键。 */
    report = ds3_report(PAD_CONN_BT);
    report.data[5] = 0x05;
    report.data[6] = 0x14;
    report.data[7] = 0x01;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_TOUCHPAD | PAD_BTN_R3 | PAD_BTN_L1 | PAD_BTN_TRIANGLE |
                        PAD_BTN_HOME));
}

static pad_report_t dualshock4_bt_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_BT;
    report.vid = 0x054C;
    report.pid = 0x05C4; /* DualShock 4 v1 */
    report.report_id = 0x11;
    report.len = 64;
    report.data[0] = 0x11;
    report.data[1] = 0xC0; /* 蓝牙报告比有线多两个前导字节 */
    report.data[3] = 0x80; /* LX / LY / RX / RY 都在中位 */
    report.data[4] = 0x80;
    report.data[5] = 0x80;
    report.data[6] = 0x80;
    report.data[7] = 0x08; /* 帽子开关松开 */
    return report;
}

/** DualShock 4 蓝牙（Report ID 0x11）与电量字节偏移（status[0] 在 0x20）。 */
static void dualshock4_bt_parses_by_pid(void)
{
    pad_report_t report = dualshock4_bt_report();
    report.data[7] = 0x28;  /* Cross + 帽子开关松开 */
    report.data[8] = 0x03;  /* L1 + R1 */
    report.data[9] = 0x01;  /* PS 键 */
    report.data[10] = 0xFF; /* L2 全按 */
    report.data[32] = 0x1A; /* 电量 10 档 + 充电中 */

    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_HOME));
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK_EQ(state.trigger[PAD_TRIGGER_R2], PAD_AXIS_MIN);
    CHECK_EQ(state.battery_percent, 100);
    CHECK(state.charging);
    CHECK(state.touch[PAD_TOUCH_LEFT].present);
}

static pad_report_t dualsense_usb_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_USB;
    report.vid = 0x054C;
    report.pid = 0x0CE6; /* DualSense */
    report.report_id = 0x01;
    report.len = 64;
    report.data[0] = 0x01;
    report.data[1] = 0x80; /* LX / LY / RX / RY 都在中位 */
    report.data[2] = 0x80;
    report.data[3] = 0x80;
    report.data[4] = 0x80;
    report.data[8] = 0x08; /* 帽子开关松开 */
    return report;
}

/**
 * DualSense / DualSense Edge 有线（Report ID 0x01）：与 DS4 有线同报 0x01，
 * 但扳机之后多一个序号字节，靠 PID 分行；键位与蓝牙的 0x31 行共用一份位序。
 */
static void dualsense_usb_parses_by_pid(void)
{
    pad_report_t report = dualsense_usb_report();
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);
    CHECK_EQ(state.buttons, 0);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);

    /* 面键、肩键、Options 与 Edge 背键与蓝牙同一份位序。 */
    report.data[8] = 0x28;  /* Cross + 帽子开关松开 */
    report.data[9] = 0x23;  /* L1 + R1 + Options */
    report.data[10] = 0xC0; /* Edge 的两颗背键 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_OPT | PAD_BTN_L4 |
                        PAD_BTN_R4));
    report.data[10] = 0x07; /* PS + 触摸板按下 + 静音 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CROSS | PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_OPT | PAD_BTN_HOME |
                        PAD_BTN_SHARE | PAD_BTN_MUTE));

    /* 摇杆从第 2 字节、扳机从第 6 字节起（序号字节在扳机之后）。 */
    report.data[1] = 0x00; /* LX 全左 */
    report.data[2] = 0xFF; /* LY 全下：报告里 0 在上，解析侧翻正 */
    report.data[5] = 0xFF; /* L2 全按 */
    report.data[6] = 0x00; /* R2 松开 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MIN);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_MIN);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK_EQ(state.trigger[PAD_TRIGGER_R2], PAD_AXIS_MIN);

    /* 运动字段在第 17 字节起：角速度 3 轴 + 加速度 3 轴，小端。 */
    report.data[24] = 0x00;
    report.data[25] = 0x1F; /* 加速度第二轴约 1 g */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.caps & PAD_CAP_MOTION, PAD_CAP_MOTION);
    CHECK(state.motion.present);
    CHECK(state.motion.accel[1] > 7000);
    CHECK(state.motion.accel[1] < 9000);

    /* Edge 有线与 DualSense 共用一行。 */
    report = dualsense_usb_report();
    report.pid = 0x0DF2;
    report.data[9] = 0x02; /* R1 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_R1);
}

/**
 * DualSense 蓝牙（Report ID 0x31）空闲帧样本（取自 pc/remapadctl.py --dump）：
 * 第 9 字节读作 0x08，正是方向键帽子开关的松开值、面键位全为 0，
 * 因此按键位图从第 9 字节起、四轴从第 2 字节起。
 */
static const uint8_t kDualSenseBtIdle[64] = {
    0x31, 0xA1, 0x7F, 0x7A, 0x7F, 0x7D, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0xAA, 0x5D,
    0xDB, 0xD2, 0xFD, 0xFF, 0xFE, 0xFF, 0x01, 0x00, 0x75, 0xFF, 0x99, 0x1F, 0xA7, 0x04, 0xE0,
    0x53, 0x1D, 0x19, 0x0E, 0x88, 0x3E, 0x90, 0x2F, 0x80, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0xA0, 0x08, 0x00, 0x09, 0x00, 0x00, 0xDE, 0x41, 0x87,
    0xBE, 0x41, 0xCA, 0xF4,
};

static pad_report_t dualsense_bt_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_BT;
    report.vid = 0x054C;
    report.pid = 0x0DF2; /* DualSense Edge */
    report.report_id = 0x31;
    report.len = (uint8_t)sizeof(kDualSenseBtIdle);
    memcpy(report.data, kDualSenseBtIdle, sizeof(kDualSenseBtIdle));
    return report;
}

static void dualsense_bt_buttons_map_by_position(void)
{
    pad_report_t report = dualsense_bt_report();
    pad_state_t state;
    pad_state_from_report(&report, &state);
    /* 空闲帧：没有按键、四轴都在死区内回到中位、扳机松开，且不走兜底布局。 */
    CHECK_EQ(state.family, PAD_FAMILY_PS);
    CHECK_EQ(state.caps & PAD_CAP_FALLBACK_LAYOUT, 0);
    CHECK_EQ(state.buttons, 0);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_RX], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_RY], PAD_AXIS_CENTER);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MIN);
    CHECK_EQ(state.trigger[PAD_TRIGGER_R2], PAD_AXIS_MIN);

    /* 面键按位置：物理 ✕ 下、○ 右、□ 左、△ 上（第 9 字节的高四位）。 */
    report.data[9] = 0x28;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CROSS);
    report.data[9] = 0x48;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CIRCLE);
    report.data[9] = 0x18;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SQUARE);
    report.data[9] = 0x88;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TRIANGLE);

    /* 肩键、Create、Options 与摇杆按下在第 10 字节。 */
    report.data[9] = 0x08;
    report.data[10] = 0x73;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_TOUCHPAD | PAD_BTN_OPT | PAD_BTN_L3));
    report.data[10] = 0x80;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_R3);

    /* PS、触摸板按下与 DualSense 的静音键在第 11 字节。 */
    report.data[10] = 0x00;
    report.data[11] = 0x07;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)(PAD_BTN_HOME | PAD_BTN_SHARE | PAD_BTN_MUTE));

    /* 帽子开关：向上只出方向键上，右上同时置两位。 */
    report.data[11] = 0x00;
    report.data[9] = 0x00;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_DPAD_UP | PAD_BTN_DPAD_DOWN | PAD_BTN_DPAD_LEFT |
                              PAD_BTN_DPAD_RIGHT),
             (uint32_t)PAD_BTN_DPAD_UP);
    report.data[9] = 0x01;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons & (PAD_BTN_DPAD_UP | PAD_BTN_DPAD_RIGHT),
             (uint32_t)(PAD_BTN_DPAD_UP | PAD_BTN_DPAD_RIGHT));
}

static void dualsense_bt_sticks_triggers_and_motion(void)
{
    pad_report_t report = dualsense_bt_report();
    report.data[2] = 0x00; /* LX 全左 */
    report.data[3] = 0xFF; /* LY 全下：报告里 0 在上、255 在下，解析侧翻正 */
    report.data[4] = 0x7F; /* RX 落在死区内 */
    report.data[5] = 0x00; /* RY 全上 */
    report.data[6] = 0xFF; /* L2 全按 */
    report.data[7] = 0x80; /* R2 半按 */
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MIN);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_MIN);
    CHECK_EQ(state.axis[PAD_AXIS_RX], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_RY], PAD_AXIS_MAX);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], PAD_AXIS_MAX);
    CHECK(state.trigger[PAD_TRIGGER_R2] > 2000);
    CHECK(state.trigger[PAD_TRIGGER_R2] < 2100);

    /* 运动字段：静止帧里三轴角速度接近 0、加速度有一轴约 1 g，偏移对不上不会成立。 */
    CHECK_EQ(state.caps & PAD_CAP_MOTION, PAD_CAP_MOTION);
    CHECK(state.motion.present);
    for (size_t i = 0; i < 3; i++) {
        CHECK(state.motion.gyro[i] > -200);
        CHECK(state.motion.gyro[i] < 200);
    }
    CHECK(state.motion.accel[1] > 7000);
    CHECK(state.motion.accel[1] < 9000);
}

static void dualsense_edge_back_buttons_map_to_gl_gr(void)
{
    pad_report_t report = dualsense_bt_report();
    pad_state_t state;

    /* 第 11 字节高两位是 DualSense Edge 的两颗背键（左 0x40、右 0x80）。 */
    report.data[11] = 0x40;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_L4);

    report.data[11] = 0x80;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_R4);

    /* 背键可与 PS / 触摸板 / 静音键同时按下；目标侧把 L4 / R4 折进 GL / GR。 */
    report.data[11] = 0xC7;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_L4 | PAD_BTN_R4 | PAD_BTN_HOME | PAD_BTN_SHARE |
                        PAD_BTN_MUTE));

    /* Fn 键（bit4 / bit5）本轮不映射：按住不出任何按键位。 */
    report.data[11] = 0x30;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, 0);
}

/** DS4 的 SHARE 与 DS5 的 Create 是左侧小键，与 Xbox 的 View、DS3 的 Select
 *  同位：按位置语义归一为减号位（PAD_BTN_TOUCHPAD）；触摸板按下作为中央
 *  额外键归一为截图位（PAD_BTN_SHARE）。此前 SHARE 按功能语义落在截图位，
 *  串流链路（Sunshine 把一颗 View 键双写成 SHARE+触摸板按下）转发到主机时
 *  一次按键同时点亮减号与截图，且没有触摸板动作的源手柄在主机侧按不出减号。
 *  有线与蓝牙、DS4 与 DS5 共用一张位图，四个形态逐一锁位。 */
static void ps_share_and_touchpad_map_by_position(void)
{
    pad_report_t report;
    pad_state_t state;

    /* DS4 有线（0x05C4）：按钮区从 b5 起，SHARE 在 b6 bit4、触摸板在 b7 bit1。 */
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_USB;
    report.vid = 0x054C;
    report.pid = 0x05C4;
    report.report_id = 0x01;
    report.len = 64;
    report.data[0] = 0x01;
    report.data[5] = 0x08; /* 帽子开关松开 */
    report.data[6] = 0x10; /* SHARE */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
    report.data[6] = 0x00;
    report.data[7] = 0x02; /* 触摸板按下 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SHARE);

    /* DS4 蓝牙（0x11）：按钮区整体后移两位（b8 bit4 / b9 bit1）。 */
    report = dualshock4_bt_report();
    report.data[8] = 0x10;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
    report.data[8] = 0x00;
    report.data[9] = 0x02;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SHARE);

    /* DualSense 有线：按钮区从 b8 起（b9 bit4 = Create、b10 bit1 = 触摸板）。 */
    report = dualsense_usb_report();
    report.data[9] = 0x10;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
    report.data[9] = 0x00;
    report.data[10] = 0x02;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SHARE);

    /* DualSense 蓝牙（0x31）：整体再后移一字节（b10 bit4 / b11 bit1）。 */
    report = dualsense_bt_report();
    report.data[10] = 0x10;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
    report.data[10] = 0x00;
    report.data[11] = 0x02;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_SHARE);
}

/**
 * 耳机状态字段的偏移与位序都要靠插拔核对（headset_style 默认
 * PAD_HEADSET_NONE）：未登记的行即便整份报文字节全是 0xFF，也必须保持
 * 「未插入」——与音频无关的字节不能被当成插入状态。
 */
static void unregistered_headset_row_reports_nothing(void)
{
    pad_report_t report = dualshock4_bt_report();
    pad_state_t state;

    memset(report.data, 0xFF, report.len);
    report.data[0] = report.report_id;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.headset_present, 0);
    CHECK_EQ(state.headset_mic, 0);

    report = ds3_report(PAD_CONN_BT);
    memset(report.data, 0xFF, report.len);
    report.data[0] = report.report_id;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.headset_present, 0);
    CHECK_EQ(state.headset_mic, 0);
}

/**
 * DualSense 蓝牙行登记了耳机状态字节（第 55 字节）：bit0 是插入、bit1 是
 * 带麦。取值来自 DualSense Edge（0x0DF2）的插拔差分：拔掉 0x00、
 * 插入 0x01、插入带麦 0x03（第 56 字节跟着 bit0 走）。
 */
static void dualsense_bt_headset_state_parses(void)
{
    pad_report_t report = dualsense_bt_report();
    pad_state_t state;

    report.data[55] = 0x00;
    report.data[56] = 0x00;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.headset_present, 0);
    CHECK_EQ(state.headset_mic, 0);

    report.data[55] = 0x01;
    report.data[56] = 0x01;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.headset_present, 1);
    CHECK_EQ(state.headset_mic, 0);

    report.data[55] = 0x03;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.headset_present, 1);
    CHECK_EQ(state.headset_mic, 1);
}

/**
 * DualSense 蓝牙行登记了电量字节（第 54 字节）：与 DS4 同一套读法，低四位是
 * 0-10 档、bit4 表示充电中。两份样本交叉核对：一份空闲帧
 * 读作 0x09（90%），后来同一只 Edge 掉到 0x05（50%），两份样本里耳机
 * 字节（第 55 字节）都在原位，偏移没有漂移。
 */
static void dualsense_bt_battery_parses(void)
{
    pad_report_t report = dualsense_bt_report();
    pad_state_t state;

    pad_state_from_report(&report, &state);
    CHECK_EQ(state.caps & PAD_CAP_BATTERY, PAD_CAP_BATTERY);
    CHECK(state.battery_present);
    CHECK_EQ(state.battery_percent, 90);
    CHECK(!state.charging);

    report.data[54] = 0x05; /* 两天后的同一只 Edge：90% → 50% */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.battery_percent, 50);
    CHECK(!state.charging);

    report.data[54] = 0x1A; /* 10 档 + 充电中 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.battery_percent, 100);
    CHECK(state.charging);
}

static pad_report_t dualshock4_usb_report(void)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    report.family = PAD_FAMILY_PS;
    report.conn = PAD_CONN_USB;
    report.vid = 0x054C;
    report.pid = 0x09CC; /* DualShock 4 v2 */
    report.report_id = 0x01;
    report.len = 64;
    report.data[0] = 0x01;
    report.data[1] = 0x80; /* LX / LY / RX / RY 都在中位 */
    report.data[2] = 0x80;
    report.data[3] = 0x80;
    report.data[4] = 0x80;
    report.data[5] = 0x08; /* 帽子开关松开 */
    return report;
}

/**
 * 触摸板：DS4 与 DualSense 的触点是 4 字节（触点字节 + 12 位 X + 12 位 Y），
 * 一帧两个触点按归一后的 X 分到左右半区，同一半区取先出现的那一路，未置
 * 触点位的路不填。四种形态各锁一次偏移——两家的偏移都取自 Linux
 * hid-playstation.c 的报告结构，尚未核对。
 */
static void ps_touch_halves_split_by_position(void)
{
    /* DS4 有线：第一个触点在偏移 35（历史份数与时间戳之后）。 */
    pad_report_t report = dualshock4_usb_report();
    report.data[35] = 0x01; /* 触点 1 */
    report.data[36] = 0x00; /* X 低 8 位 */
    report.data[37] = 0x00; /* X 高 4 位 + Y 低 4 位 */
    report.data[38] = 0x00; /* Y 高 8 位 */
    report.data[39] = 0x80; /* 第二路没有触点 */
    pad_state_t state;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.caps & PAD_CAP_TOUCHPAD, PAD_CAP_TOUCHPAD);
    CHECK(state.touch[PAD_TOUCH_LEFT].pressed);
    CHECK_EQ(state.touch[PAD_TOUCH_LEFT].x, PAD_AXIS_MIN);
    CHECK(!state.touch[PAD_TOUCH_RIGHT].pressed);

    /* 第二路放在右半：X = 1919（最右）。 */
    report.data[39] = 0x02;
    report.data[40] = 0x7F;
    report.data[41] = 0x07; /* X 高 4 位 = 7 → X = 0x77F */
    report.data[42] = 0x00;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].raw_x, 1919);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].x, PAD_AXIS_MAX);

    /* 触点位（bit7）置位表示这一路没有手指：右半回到未按下。 */
    report.data[39] = 0x82;
    pad_state_from_report(&report, &state);
    CHECK(!state.touch[PAD_TOUCH_RIGHT].pressed);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].x, PAD_AXIS_MIN);

    /* 同一半区两路触点：保留先出现的那一路（第一路的 X = 0）。 */
    report.data[39] = 0x03; /* 触点 3 也在左半：X = 0x140 */
    report.data[40] = 0x40;
    report.data[41] = 0x01;
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.touch[PAD_TOUCH_LEFT].raw_x, 0);
    CHECK(!state.touch[PAD_TOUCH_RIGHT].pressed);

    /* DualSense 有线：第一个触点在偏移 33；X = 960 正好落在半区边界上，
     * 归一到中点 2048 后归右半。 */
    report = dualsense_usb_report();
    report.data[33] = 0x00;
    report.data[34] = 0xC0; /* X 低 8 位 */
    report.data[35] = 0x03; /* X 高 4 位 = 3 → X = 960 */
    report.data[36] = 0x00;
    report.data[37] = 0x80; /* 第二路没有触点 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.caps & PAD_CAP_TOUCHPAD, PAD_CAP_TOUCHPAD);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].raw_x, 960);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].x, PAD_AXIS_CENTER);
    CHECK(!state.touch[PAD_TOUCH_LEFT].pressed);

    /* DualSense 蓝牙：第一个触点在偏移 34（比有线整体后移一位）。 */
    report = dualsense_bt_report();
    report.data[34] = 0x00;
    report.data[35] = 0x08; /* X 低 8 位 */
    report.data[36] = 0x07; /* X 高 4 位 = 7 → X = 1800 */
    report.data[37] = 0x00;
    report.data[38] = 0x80; /* 第二路没有触点 */
    pad_state_from_report(&report, &state);
    CHECK_EQ(state.touch[PAD_TOUCH_RIGHT].raw_x, 1800);
    CHECK(state.touch[PAD_TOUCH_RIGHT].x > PAD_AXIS_CENTER);

    /* DS4 蓝牙：第一个触点在偏移 37。 */
    report = dualshock4_bt_report();
    report.data[37] = 0x00;
    report.data[38] = 0x00;
    report.data[39] = 0x00;
    report.data[40] = 0x00;
    report.data[41] = 0x80; /* 第二路没有触点 */
    pad_state_from_report(&report, &state);
    CHECK(state.touch[PAD_TOUCH_LEFT].pressed);
    CHECK_EQ(state.touch[PAD_TOUCH_LEFT].x, PAD_AXIS_MIN);
}

HOST_TEST_SUITE(suite_pad_device, "pad_device",
                {"Xbox 面键按位置映射（物理 A 下 → ✕、物理 B 右 → ○）",
                 xbox_face_buttons_map_by_position},
                {"Xbox 方向键、肩键与摇杆量程", xbox_dpad_shoulders_and_sticks},
                {"PS 报告：帽子开关、面键、电量、触摸板与静音键",
                 ps_report_parses_hat_face_buttons_and_battery},
                {"摇杆死区与 Y 轴方向", stick_deadzone_and_y_direction},
                {"未识别型号回落 Xbox 布局并标记兜底", unknown_model_falls_back_to_xbox_layout},
                {"VID 判定家族（Steam 布局未定，走兜底）", family_detection_and_steam_gap},
                {"DS3 有线与蓝牙都按 PS 键位解析", dualshock3_parses_on_usb_and_bt},
                {"DS4 蓝牙按键与电量按 PID 匹配", dualshock4_bt_parses_by_pid},
                {"DualSense 有线按 PID 与 DS4 有线分开解析", dualsense_usb_parses_by_pid},
                {"DualSense 蓝牙按键不再乱配（面键、方向键、肩键、静音键）",
                 dualsense_bt_buttons_map_by_position},
                {"DualSense 蓝牙摇杆、扳机与运动字段量程",
                 dualsense_bt_sticks_triggers_and_motion},
                {"DualSense Edge 背键能当 GL / GR 用",
                 dualsense_edge_back_buttons_map_to_gl_gr},
                {"PS 的 SHARE/Create 与触摸板按位置归一（左小键 → 减号、触摸板 → 截图）",
                 ps_share_and_touchpad_map_by_position},
                {"未登记耳机偏移的行不上报耳机状态",
                 unregistered_headset_row_reports_nothing},
                {"DualSense 蓝牙耳机状态按第 55 字节解析",
                 dualsense_bt_headset_state_parses},
                {"DualSense 蓝牙电量按第 54 字节解析（低四位 0-10 档、bit4 充电）",
                 dualsense_bt_battery_parses},
                {"PS 触摸点按左右半区分流（DS4 有线/蓝牙、DualSense 有线/蓝牙）",
                 ps_touch_halves_split_by_position});

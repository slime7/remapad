#include "pad_device.h"

#include <string.h>

/** 字段偏移缺省值：该布局没有这个字段。 */
#define PAD_OFF_NONE 0xFF

/** 摇杆死区（半量程的百分比）：小于它的偏移按中位处理。 */
#define PAD_STICK_DEADZONE_PERCENT 8
#define PAD_STICK_DEADZONE ((PAD_AXIS_CENTER * PAD_STICK_DEADZONE_PERCENT) / 100)

/** 摇杆原始格式。 */
typedef enum {
    PAD_STICK_U8 = 0, /**< 单字节，中心 0x80（PS、Steam 原生报告）。 */
    PAD_STICK_I16,    /**< 有符号 16 位小端，中心 0（Xbox）。 */
} pad_stick_style_t;

/**
 * 家族布局表的一行：按（家族, Report ID, 连接方式, PID）定位字段偏移。偏移
 * 一律从收到的报告首字节起算（含 Report ID）。
 *
 * 同一组合下有多个型号时报 PID 分行（PS 系三种型号都报 0x01，但字段偏移各不
 * 相同）；布局相同的多个 PID 写在同一行的 pids 里；pids 为空表示该组合下所有
 * 型号共用这行。报告未带 PID（离线构造或旧帧）时不做型号过滤，取该组合的第一
 * 行。新增手柄只加一行，不改解析代码。
 *
 * 现有各行的偏移取自公开资料，实机接线时用 `pc/bridge.py --dump` 抓包核对；
 * 偏差只影响这张表，不影响上下游。
 */
typedef struct {
    pad_family_t family;
    /** 连接方式；PAD_CONN_UNKNOWN 表示有线与蓝牙共用这一行。 */
    pad_conn_t conn;
    uint8_t report_id;
    /** 该行适用的 PID，0 结尾；首元素为 0 表示不按型号过滤。 */
    uint16_t pids[4];
    uint8_t buttons_off;
    uint8_t buttons_bytes;
    /** 方向键帽子开关偏移；0xFF 表示方向键在按键位图里。 */
    uint8_t hat_off;
    uint8_t trigger_off[PAD_TRIGGER_COUNT];
    uint8_t stick_off[PAD_AXIS_COUNT];
    uint8_t touch_off;
    uint8_t motion_off;
    uint8_t battery_off;
    uint16_t touch_max_x;
    uint16_t touch_max_y;
    pad_stick_style_t stick_style;
    uint32_t caps;
    /** 设备 Y 轴向下为正时置位，解析侧翻成「上为正」。 */
    bool invert_y;
    const uint32_t *btn_map;
} pad_layout_t;

/** Xbox：bit0-3 方向键、bit4 Menu、bit5 View（select）、bit6/7 按下摇杆、
 *  bit8/9 肩键、bit10 西瓜键、bit11 分享键（Series 手柄才有，位置按公开
 *  资料填，待抓包核对）、bit12-15 面键（物理 A 在下、B 在右）。 */
static const uint32_t s_xbox_btn_map[16] = {
    PAD_BTN_DPAD_UP, PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_LEFT, PAD_BTN_DPAD_RIGHT,
    PAD_BTN_OPT, PAD_BTN_TOUCHPAD, PAD_BTN_L3, PAD_BTN_R3,
    PAD_BTN_L1, PAD_BTN_R1, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_SQUARE, PAD_BTN_TRIANGLE,
};

/** PS：字节 0 低四位是方向键帽子开关（下方单独展开），
 *  面键就是私有格式的四个键位（Square 左、Cross 下、Circle 右、Triangle 上）；
 *  Create 与 Options 填分享与选项位，PS 键填主页位，触摸板按下填触摸板位，
 *  DualSense 的静音键填静音位；第三字节的高两位是 DualSense Edge 的两颗背键
 *  （实测抓包：bit6 左、bit7 右），填进 L4 / R4，目标侧按侧折进 GL / GR。
 *  Edge 的左右 Fn 键（同一字节 bit4 / bit5）本轮不映射——它们兼作配置档
 *  切换的组合修饰键。DS4 没有这些按键，对应位按公开资料恒为 0。 */
static const uint32_t s_ps_btn_map[24] = {
    0, 0, 0, 0,
    PAD_BTN_SQUARE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_TRIANGLE,
    PAD_BTN_L1, PAD_BTN_R1, 0, 0,
    PAD_BTN_SHARE, PAD_BTN_OPT, PAD_BTN_L3, PAD_BTN_R3,
    PAD_BTN_HOME, PAD_BTN_TOUCHPAD, PAD_BTN_MUTE, 0,
    0, 0, PAD_BTN_L4, PAD_BTN_R4,
};

/** DualShock 3（有线与蓝牙同布局）：按键全在按键位图里，方向键也是——
 *  没有帽子开关。Select 填触摸板位（目标侧作减号）、Start 填选项位，与
 *  PS 家族其余型号的位置语义一致。L2 / R2 的数字位不映射：扳机走同字的
 *  压力值（第 12、13 字节）。 */
static const uint32_t s_ps3_btn_map[24] = {
    PAD_BTN_TOUCHPAD, PAD_BTN_L3, PAD_BTN_R3, PAD_BTN_OPT,
    PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_LEFT,
    0, 0, PAD_BTN_L1, PAD_BTN_R1,
    PAD_BTN_TRIANGLE, PAD_BTN_CIRCLE, PAD_BTN_CROSS, PAD_BTN_SQUARE,
    PAD_BTN_HOME, 0, 0, 0,
    0, 0, 0, 0,
};

/**
 * 家族表。Steam 原生布局（lizard 模式）尚未实机抓包，暂不登记：该族按
 * 未知型号路径走 Xbox 兜底并在能力位里标记 PAD_CAP_FALLBACK_LAYOUT，接入
 * 手柄时先用 `pc/bridge.py --dump` 抓包，再把偏移补成一行。
 */
static const pad_layout_t s_layouts[] = {
    {
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_USB,
        .report_id = 0x00,
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {3, 4},
        .stick_off = {5, 7, 9, 11},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_I16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        /* Xbox 的 Y 轴沿用 XInput 语义（正为上），不需要翻转。 */
        .invert_y = false,
        .btn_map = s_xbox_btn_map,
    },
    {
        /* Xbox 蓝牙：字段顺序与有线一致，偏移待抓包核对。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {3, 4},
        .stick_off = {5, 7, 9, 11},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_I16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = false,
        .btn_map = s_xbox_btn_map,
    },
    {
        /* DualShock 4 有线（Report ID 0x01）。DualSense 有线同样报 0x01，但
         * 在扳机之后多一个序号字节，与 DS4 分成两行用 PID 区分。
         * 电量取 status[0]（0x1E）：低四位是 0-10 档、bit4 表示充电中。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x05C4, 0x09CC},
        .buttons_off = 5,
        .buttons_bytes = 3,
        .hat_off = 5,
        .trigger_off = {8, 9},
        .stick_off = {1, 2, 3, 4},
        .touch_off = 34,
        .motion_off = 13,
        .battery_off = 30,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
    },
    {
        /* DualShock 4 蓝牙（Report ID 0x11）：比有线多两个前导字节，其余偏移
         * 整体后移两位（电量在 0x20）。DualSense 蓝牙另报 0x31，见后两行。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x11,
        .pids = {0x05C4, 0x09CC},
        .buttons_off = 7,
        .buttons_bytes = 3,
        .hat_off = 7,
        .trigger_off = {10, 11},
        .stick_off = {3, 4, 5, 6},
        .touch_off = 36,
        .motion_off = 15,
        .battery_off = 32,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
    },
    {
        /* DualSense 与 DualSense Edge 有线（Report ID 0x01，64 字节）：位序与
         * 蓝牙的 0x31 行相同，但比它少两个字节前缀（第 8 字节低四位是方向键
         * 帽子开关、高四位是面键；第 9 字节是肩键、Create/Options 与摇杆按下；
         * 第 10 字节是 PS、触摸板按下与静音键，Edge 的背键在第三字节高两位）。
         * 偏移由蓝牙那行的实测值减去两字节前缀换算，抓包核对前作初值；触摸点
         * （每点 4 字节）与电量字节尚未核对，本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 8,
        .buttons_bytes = 3,
        .hat_off = 8,
        .trigger_off = {5, 6},
        .stick_off = {1, 2, 3, 4},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 16,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
    },
    {
        /* DualSense 与 DualSense Edge 蓝牙（Report ID 0x31）：比 DS4 蓝牙的
         * 0x11 布局整体后移一位。偏移为 DualSense Edge（054C:0DF2）实测抓包：
         * 静止帧第 9 字节读作 0x08（帽子开关松开）、四轴落在死区内、第 17-22
         * 字节的角速度接近 0 而加速度有一轴约 1 g。触摸板每点 4 字节、电量
         * 字节与 DS4 不同，两处都还没核对，本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x31,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 9,
        .buttons_bytes = 3,
        .hat_off = 9,
        .trigger_off = {6, 7},
        .stick_off = {2, 3, 4, 5},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 17,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
    },
    {
        /* DualShock 3（0x054C:0x0268）：有线与蓝牙都报 0x01、字段偏移一致
         * （蓝牙多一层传输头，剥掉后与有线相同），因此一行覆盖两种连接。
         * 按键位图从第 5 字节起三字节，没有帽子开关；运动字段是 41-46 的大端
         * 加速度加 47-48 的陀螺，与解析器要求的 6×int16 小端不同，电量也不在
         * 输入报告里（要靠特性报告查询），两处都不登记。
         * 按键位是否为低电平有效（0 表示按下）与蓝牙是否多一字节前缀，都要等
         * `pc/bridge.py --dump` 实测确认，当前按高电平有效、49 字节形式登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x01,
        .pids = {0x0268},
        .buttons_off = 5,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {12, 13},
        .stick_off = {1, 2, 3, 4},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ps3_btn_map,
    },
};

/** 未识别型号的兜底布局：按 Xbox 有线解析，能力位另行标记。 */
static const pad_layout_t s_fallback_layout = {
    .family = PAD_FAMILY_XBOX,
    .conn = PAD_CONN_UNKNOWN,
    .report_id = 0x00,
    .buttons_off = 1,
    .buttons_bytes = 2,
    .hat_off = PAD_OFF_NONE,
    .trigger_off = {3, 4},
    .stick_off = {5, 7, 9, 11},
    .touch_off = PAD_OFF_NONE,
    .motion_off = PAD_OFF_NONE,
    .battery_off = PAD_OFF_NONE,
    .stick_style = PAD_STICK_I16,
    .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
    .invert_y = false,
    .btn_map = s_xbox_btn_map,
};

static bool range_ok(const pad_report_t *report, uint8_t off, uint8_t width)
{
    return off != PAD_OFF_NONE && (uint16_t)off + width <= (uint16_t)report->len;
}

static int16_t read_i16(const pad_report_t *report, uint8_t off)
{
    return (int16_t)((uint16_t)report->data[off] | ((uint16_t)report->data[off + 1] << 8));
}

static uint16_t norm_u8(uint8_t value)
{
    return (uint16_t)(((uint32_t)value * PAD_AXIS_MAX + 127u) / 255u);
}

static uint16_t norm_i16(int16_t value)
{
    int32_t scaled = ((int32_t)value * PAD_AXIS_MAX) / 32767;
    if (scaled > (int32_t)PAD_AXIS_CENTER - 1) {
        scaled = (int32_t)PAD_AXIS_CENTER - 1;
    }
    if (scaled < -(int32_t)PAD_AXIS_CENTER) {
        scaled = -(int32_t)PAD_AXIS_CENTER;
    }
    return (uint16_t)((int32_t)PAD_AXIS_CENTER + scaled);
}

static uint16_t apply_stick_deadzone(uint16_t value)
{
    const int32_t center = PAD_AXIS_CENTER;
    int32_t delta = (int32_t)value - center;
    if (delta <= PAD_STICK_DEADZONE && delta >= -PAD_STICK_DEADZONE) {
        return (uint16_t)center;
    }
    /* 越过死区后把剩余行程重新铺满，避免边缘出现台阶。 */
    if (delta > 0) {
        delta = ((delta - PAD_STICK_DEADZONE) * center + (center - PAD_STICK_DEADZONE) / 2) /
                (center - PAD_STICK_DEADZONE);
        if (delta > center - 1) {
            delta = center - 1;
        }
    } else {
        delta = ((delta + PAD_STICK_DEADZONE) * center - (center - PAD_STICK_DEADZONE) / 2) /
                (center - PAD_STICK_DEADZONE);
        if (delta < -center) {
            delta = -center;
        }
    }
    return (uint16_t)(center + delta);
}

/** 方向键帽子开关（0 上、顺时针，8 及以上为松开）展开成四个方向位。 */
static uint32_t hat_buttons(uint8_t hat)
{
    switch (hat & 0x0Fu) {
    case 0:
        return PAD_BTN_DPAD_UP;
    case 1:
        return PAD_BTN_DPAD_UP | PAD_BTN_DPAD_RIGHT;
    case 2:
        return PAD_BTN_DPAD_RIGHT;
    case 3:
        return PAD_BTN_DPAD_RIGHT | PAD_BTN_DPAD_DOWN;
    case 4:
        return PAD_BTN_DPAD_DOWN;
    case 5:
        return PAD_BTN_DPAD_DOWN | PAD_BTN_DPAD_LEFT;
    case 6:
        return PAD_BTN_DPAD_LEFT;
    case 7:
        return PAD_BTN_DPAD_UP | PAD_BTN_DPAD_LEFT;
    default:
        return 0;
    }
}

pad_family_t pad_family_from_ids(uint16_t vid, uint16_t pid)
{
    (void)pid;
    switch (vid) {
    case 0x045E:
        return PAD_FAMILY_XBOX; /* Microsoft */
    case 0x054C:
        return PAD_FAMILY_PS; /* Sony */
    case 0x28DE:
        return PAD_FAMILY_STEAM; /* Valve */
    default:
        return PAD_FAMILY_UNKNOWN;
    }
}

/** 该行是否适用于这个 PID；行的 pids 为空表示不按型号过滤。 */
static bool layout_pid_match(const pad_layout_t *layout, uint16_t pid)
{
    if (layout->pids[0] == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(layout->pids) / sizeof(layout->pids[0]); i++) {
        if (layout->pids[i] == pid) {
            return true;
        }
    }
    return false;
}

static const pad_layout_t *find_layout(const pad_report_t *report, pad_family_t *family)
{
    *family = report->family;
    if (*family == PAD_FAMILY_UNKNOWN) {
        *family = pad_family_from_ids(report->vid, report->pid);
    }
    for (size_t i = 0; i < sizeof(s_layouts) / sizeof(s_layouts[0]); i++) {
        const pad_layout_t *layout = &s_layouts[i];
        if (layout->family != *family) {
            continue;
        }
        if (layout->report_id != report->report_id) {
            continue;
        }
        /* 行的连接方式为空表示两种连接共用；报告的连接方式为空表示不做过滤。 */
        if (layout->conn != PAD_CONN_UNKNOWN && report->conn != PAD_CONN_UNKNOWN &&
            layout->conn != report->conn) {
            continue;
        }
        /* 型号不匹配就继续找同一组合下的下一行；报告没带 PID 时不做过滤。 */
        if (report->pid != 0 && !layout_pid_match(layout, report->pid)) {
            continue;
        }
        return layout;
    }
    return NULL;
}

static void parse_buttons(const pad_report_t *report, const pad_layout_t *layout,
                          pad_state_t *state)
{
    uint32_t buttons = 0;
    if (range_ok(report, layout->buttons_off, layout->buttons_bytes)) {
        const uint8_t bits = (uint8_t)(layout->buttons_bytes * 8u);
        for (uint8_t bit = 0; bit < bits; bit++) {
            const uint8_t byte = report->data[layout->buttons_off + bit / 8u];
            if (((byte >> (bit % 8u)) & 0x01u) == 0) {
                continue;
            }
            buttons |= layout->btn_map[bit];
        }
    }
    if (range_ok(report, layout->hat_off, 1)) {
        buttons |= hat_buttons(report->data[layout->hat_off]);
    }
    state->buttons = buttons;
}

static void parse_axes(const pad_report_t *report, const pad_layout_t *layout,
                       pad_state_t *state)
{
    for (size_t axis = 0; axis < PAD_AXIS_COUNT; axis++) {
        uint16_t value = PAD_AXIS_CENTER;
        const uint8_t off = layout->stick_off[axis];
        if (layout->stick_style == PAD_STICK_I16) {
            if (range_ok(report, off, 2)) {
                value = norm_i16(read_i16(report, off));
            }
        } else if (range_ok(report, off, 1)) {
            value = norm_u8(report->data[off]);
        }
        const bool invert = layout->invert_y && (axis == PAD_AXIS_LY || axis == PAD_AXIS_RY);
        if (invert) {
            value = (uint16_t)(PAD_AXIS_MAX - value);
        }
        state->axis[axis] = apply_stick_deadzone(value);
    }
    for (size_t i = 0; i < PAD_TRIGGER_COUNT; i++) {
        if (range_ok(report, layout->trigger_off[i], 1)) {
            state->trigger[i] = norm_u8(report->data[layout->trigger_off[i]]);
        }
    }
}

static void parse_motion(const pad_report_t *report, const pad_layout_t *layout,
                         pad_state_t *state)
{
    if (!range_ok(report, layout->motion_off, 12)) {
        return;
    }
    for (size_t i = 0; i < 6; i++) {
        const uint8_t off = (uint8_t)(layout->motion_off + i * 2u);
        const int16_t value = read_i16(report, off);
        if (i < 3) {
            state->motion.gyro[i] = value;
        } else {
            state->motion.accel[i - 3] = value;
        }
    }
    state->motion.present = true;
}

static void parse_touch(const pad_report_t *report, const pad_layout_t *layout,
                        pad_state_t *state)
{
    if (!range_ok(report, layout->touch_off, 3)) {
        return;
    }
    const uint8_t b0 = report->data[layout->touch_off];
    const uint8_t b1 = report->data[layout->touch_off + 1];
    const uint8_t b2 = report->data[layout->touch_off + 2];
    pad_touch_t *touch = &state->touch[PAD_TOUCH_LEFT];
    touch->raw_x = (uint16_t)(b0 | ((uint16_t)(b1 & 0x0Fu) << 8));
    touch->raw_y = (uint16_t)((b1 >> 4) | ((uint16_t)b2 << 4));
    touch->present = true;
    touch->pressed = (b2 & 0x80u) == 0;
    if (layout->touch_max_x > 0 && layout->touch_max_y > 0) {
        touch->x = (uint16_t)(((uint32_t)touch->raw_x * PAD_AXIS_MAX) / layout->touch_max_x);
        touch->y = (uint16_t)(((uint32_t)touch->raw_y * PAD_AXIS_MAX) / layout->touch_max_y);
    }
}

static void parse_battery(const pad_report_t *report, const pad_layout_t *layout,
                          pad_state_t *state)
{
    if (!range_ok(report, layout->battery_off, 1)) {
        return;
    }
    /* PS 报告的电量字节：低四位是 0-10 档，bit4 表示充电中。 */
    const uint8_t raw = report->data[layout->battery_off];
    const uint8_t level = raw & 0x0Fu;
    state->battery_present = true;
    state->battery_percent = level >= 10u ? 100u : (uint8_t)(level * 10u);
    state->charging = (raw & 0x10u) != 0;
}

void pad_state_from_report(const pad_report_t *report, pad_state_t *state)
{
    pad_state_defaults(state);
    state->conn = report->conn;
    state->vid = report->vid;
    state->pid = report->pid;
    state->report_id = report->report_id;
    state->report_len = report->len;
    state->seq = report->seq;
    state->family = report->family;

    if (report->len == 0 || report->len > PAD_REPORT_MAX) {
        state->caps = PAD_CAP_FALLBACK_LAYOUT;
        return;
    }

    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *layout = find_layout(report, &family);
    const bool fallback = layout == NULL;
    if (layout == NULL) {
        layout = &s_fallback_layout;
    }
    state->family = family;
    state->caps = layout->caps;
    if (fallback) {
        state->caps |= PAD_CAP_FALLBACK_LAYOUT;
    }

    parse_buttons(report, layout, state);
    parse_axes(report, layout, state);
    parse_motion(report, layout, state);
    parse_touch(report, layout, state);
    parse_battery(report, layout, state);
}

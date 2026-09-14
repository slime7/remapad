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
 * 家族布局表的一行：按（家族, Report ID, 连接方式）定位字段偏移。偏移一律
 * 从收到的报告首字节起算（含 Report ID）。新增手柄只加一行，不改解析代码。
 *
 * 现有各行的偏移取自公开资料，实机接线时用 `pc/bridge.py --dump` 抓包核对；
 * 偏差只影响这张表，不影响上下游。
 */
typedef struct {
    pad_family_t family;
    pad_conn_t conn;
    uint8_t report_id;
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
    PAD_BTN_OPT, PAD_BTN_TOUCHPAD, PAD_BTN_LSTICK, PAD_BTN_RSTICK,
    PAD_BTN_LB, PAD_BTN_RB, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_SQUARE, PAD_BTN_TRIANGLE,
};

/** PS：字节 0 低四位是方向键帽子开关（下方单独展开），
 *  面键就是私有格式的四个键位（Square 左、Cross 下、Circle 右、Triangle 上）；
 *  Create 与 Options 填分享与选项位，PS 键填主页位，触摸板按下填触摸板位，
 *  DualSense 的静音键填静音位。 */
static const uint32_t s_ps_btn_map[24] = {
    0, 0, 0, 0,
    PAD_BTN_SQUARE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_TRIANGLE,
    PAD_BTN_LB, PAD_BTN_RB, 0, 0,
    PAD_BTN_SHARE, PAD_BTN_OPT, PAD_BTN_LSTICK, PAD_BTN_RSTICK,
    PAD_BTN_HOME, PAD_BTN_TOUCHPAD, PAD_BTN_MUTE, 0,
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
        /* DualShock 4 / DualSense 有线（Report ID 0x01）：面键、摇杆、扳机、
         * 触摸板按下与静音键（DualSense 才有）的位置两者一致；电量、运动与
         * 触摸板坐标的偏移按 DualShock 4 的资料填，DualSense 这几处不同，
         * 待抓包后按 PID 分行（见 ROADMAP 的家族表回填）。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .buttons_off = 5,
        .buttons_bytes = 3,
        .hat_off = 5,
        .trigger_off = {8, 9},
        .stick_off = {1, 2, 3, 4},
        .touch_off = 34,
        .motion_off = 13,
        .battery_off = 12,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
    },
    {
        /* DualShock 4 蓝牙（Report ID 0x11）：比有线多两个前导字节。
         * DualSense 蓝牙的 Report ID 与 DS4 不同（公开资料为 0x31），本轮未登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x11,
        .buttons_off = 7,
        .buttons_bytes = 3,
        .hat_off = 7,
        .trigger_off = {10, 11},
        .stick_off = {3, 4, 5, 6},
        .touch_off = 36,
        .motion_off = 15,
        .battery_off = 14,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = s_ps_btn_map,
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
        if (report->conn != PAD_CONN_UNKNOWN && layout->conn != report->conn) {
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

#include "pad_device.h"

#include <string.h>

#include "layout.h"

/** 摇杆死区（半量程的百分比）：小于它的偏移按中位处理。 */
#define PAD_STICK_DEADZONE_PERCENT 8
#define PAD_STICK_DEADZONE ((PAD_AXIS_CENTER * PAD_STICK_DEADZONE_PERCENT) / 100)

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

/** 12 位紧凑打包（NS2 的摇杆形态）解出 X 与 Y，量程本身就是 0-4095。 */
static uint16_t unpack_u12_x(const uint8_t *packed)
{
    return (uint16_t)(packed[0] | (((uint16_t)packed[1] & 0x0Fu) << 8));
}

static uint16_t unpack_u12_y(const uint8_t *packed)
{
    return (uint16_t)((packed[1] >> 4) | ((uint16_t)packed[2] << 4));
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
    case 0x057E:
        return PAD_FAMILY_NS; /* Nintendo：真手柄与伪装成 NS 布局的第三方手柄 */
    default:
        return PAD_FAMILY_UNKNOWN;
    }
}

const char *pad_family_name(pad_family_t family)
{
    switch (family) {
    case PAD_FAMILY_XBOX:
        return "xbox";
    case PAD_FAMILY_PS:
        return "ps";
    case PAD_FAMILY_STEAM:
        return "steam";
    case PAD_FAMILY_NS:
        return "ns";
    default:
        return "unknown";
    }
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
    uint16_t raw[PAD_AXIS_COUNT] = {PAD_AXIS_CENTER, PAD_AXIS_CENTER, PAD_AXIS_CENTER,
                                    PAD_AXIS_CENTER};
    if (layout->stick_style == PAD_STICK_U12) {
        /* 左右各三字节：X 与 Y 共用打包块，因此按轴对读取（块起点写在 LX / RX）。 */
        static const size_t pairs[2][2] = {{PAD_AXIS_LX, PAD_AXIS_LY},
                                           {PAD_AXIS_RX, PAD_AXIS_RY}};
        for (size_t p = 0; p < 2; p++) {
            const uint8_t off = layout->stick_off[pairs[p][0]];
            if (!range_ok(report, off, 3)) {
                continue;
            }
            raw[pairs[p][0]] = unpack_u12_x(&report->data[off]);
            raw[pairs[p][1]] = unpack_u12_y(&report->data[off]);
        }
    } else {
        for (size_t axis = 0; axis < PAD_AXIS_COUNT; axis++) {
            const uint8_t off = layout->stick_off[axis];
            if (layout->stick_style == PAD_STICK_I16) {
                if (range_ok(report, off, 2)) {
                    raw[axis] = norm_i16(read_i16(report, off));
                }
            } else if (range_ok(report, off, 1)) {
                raw[axis] = norm_u8(report->data[off]);
            }
        }
    }
    for (size_t axis = 0; axis < PAD_AXIS_COUNT; axis++) {
        uint16_t value = raw[axis];
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

/**
 * 按行的轴映射取一路轴：全零映射表示恒等（X→X、Y→Y、Z→Z），这也是各家族
 * 的默认形态；invert 的对应位置位时取反。
 */
static void map_axis(int16_t out[3], const int16_t src[3], const uint8_t map[3],
                     uint8_t invert)
{
    const bool identity = map[0] == 0 && map[1] == 0 && map[2] == 0;
    for (size_t i = 0; i < 3; i++) {
        const uint8_t from = identity ? (uint8_t)i : map[i];
        int16_t value = from < 3 ? src[from] : 0;
        if ((invert & (uint8_t)(1u << i)) != 0) {
            value = (int16_t)(-value);
        }
        out[i] = value;
    }
}

static void parse_motion(const pad_report_t *report, const pad_layout_t *layout,
                         pad_state_t *state)
{
    if (layout->motion_off == PAD_OFF_NONE) {
        return;
    }
    const uint8_t samples = layout->motion.samples == 0 ? 1 : layout->motion.samples;
    const uint8_t stride = layout->motion.stride == 0 ? 12 : layout->motion.stride;
    if (!range_ok(report, layout->motion_off, (uint8_t)(samples * stride))) {
        return;
    }
    /* 一次带多份样本的型号（NS1 三份）取最后一份，它是最新的采样。 */
    const uint8_t base = (uint8_t)(layout->motion_off + (uint8_t)(samples - 1) * stride);
    int16_t gyro[3];
    int16_t accel[3];
    for (size_t i = 0; i < 3; i++) {
        gyro[i] = read_i16(report, (uint8_t)(base + i * 2u));
        accel[i] = read_i16(report, (uint8_t)(base + 6u + i * 2u));
    }
    /* invert_mask 的 bit0-2 对应陀螺 X/Y/Z，bit3-5 对应加速 X/Y/Z。 */
    map_axis(state->motion.gyro, gyro, layout->motion.gyro_src,
             (uint8_t)(layout->motion.invert_mask & 0x07u));
    map_axis(state->motion.accel, accel, layout->motion.accel_src,
             (uint8_t)((layout->motion.invert_mask >> 3) & 0x07u));
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
    const uint8_t raw = report->data[layout->battery_off];
    state->battery_present = true;
    if (layout->battery_style == PAD_BATTERY_NS2) {
        /* NS2 电源字节：bit0 外部供电、bit1 充电中、bits2-5 电量等级 0-9。 */
        const uint8_t level = (uint8_t)((raw >> 2) & 0x0Fu);
        state->battery_percent = level >= 9u ? 100u : (uint8_t)(level * 100u / 9u);
        state->charging = (raw & 0x02u) != 0;
        return;
    }
    /* PS 报告的电量字节：低四位是 0-10 档，bit4 表示充电中。 */
    const uint8_t level = raw & 0x0Fu;
    state->battery_percent = level >= 10u ? 100u : (uint8_t)(level * 10u);
    state->charging = (raw & 0x10u) != 0;
}

/**
 * 3.5mm 耳机状态：PS 系的音频状态字节 bit0 是插入、bit1 是带麦。偏移与位序
 * 都要靠实机插拔核对，因此只有显式登记了 headset_style 的行才解析（默认
 * PAD_HEADSET_NONE 时主机看到的就是「未插入」）；核对方法见 pc/README.md。
 */
static void parse_headset(const pad_report_t *report, const pad_layout_t *layout,
                          pad_state_t *state)
{
    if (layout->headset_style != PAD_HEADSET_PS || !range_ok(report, layout->headset_off, 1)) {
        return;
    }
    const uint8_t raw = report->data[layout->headset_off];
    state->headset_present = (raw & 0x01u) != 0;
    state->headset_mic = (raw & 0x02u) != 0;
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
    const pad_layout_t *layout = pad_layout_find(report, &family);
    const bool fallback = layout == NULL;
    if (fallback) {
        layout = pad_layout_fallback();
    }
    state->family = family;
    state->caps = layout->caps;
    if (fallback) {
        state->caps |= PAD_CAP_FALLBACK_LAYOUT;
    }
    /* 设备自带报告语言与期望身份：透传路径按它们决定能否原样转发。 */
    state->native_lang = layout->native_lang;
    state->native_identity = layout->native_identity;
    if (layout->native_lang != PAD_LANG_NONE && report->report_id != 0 &&
        report->report_id == report->data[0] && report->len <= PAD_RAW_MAX) {
        memcpy(state->raw, report->data, report->len);
        state->raw_len = report->len;
        state->raw_report_id = report->report_id;
    }

    parse_buttons(report, layout, state);
    parse_axes(report, layout, state);
    parse_motion(report, layout, state);
    parse_touch(report, layout, state);
    parse_battery(report, layout, state);
    parse_headset(report, layout, state);
}

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

    parse_buttons(report, layout, state);
    parse_axes(report, layout, state);
    parse_motion(report, layout, state);
    parse_touch(report, layout, state);
    parse_battery(report, layout, state);
}


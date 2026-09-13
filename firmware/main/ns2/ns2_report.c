#include "ns2_report.h"

#include <string.h>

/** 把规范化按键位图中的一个键摆到目标字节的指定位上。 */
static uint8_t btn_bit(uint32_t buttons, uint32_t mask, uint8_t shift)
{
    return (uint8_t)(((buttons & mask) != 0) << shift);
}

void ns2_state_for_identity(ns2_controller_state_t *out,
                            const ns2_controller_state_t *in, uint8_t identity)
{
    *out = *in;
    if (identity == NS2_ID_PRO) {
        return;
    }
    /* JoyCon 组合按左右分摊同一份规范化状态：左半保留 L 侧按键、十字键与
     * 左摇杆，右半保留 A/B/X/Y 与右摇杆；GL/GR 近似对应导轨 SL/SR。单只
     * JoyCon 2 无 NFC，状态字节清零；电池/震动特性两半一致。 */
    if (identity == NS2_ID_JOYCON_L) {
        out->buttons &= NS2_BTN_L | NS2_BTN_ZL | NS2_BTN_MINUS | NS2_BTN_CAPTURE |
                        NS2_BTN_LSTICK | NS2_BTN_GL |
                        NS2_BTN_DPAD_UP | NS2_BTN_DPAD_DOWN |
                        NS2_BTN_DPAD_LEFT | NS2_BTN_DPAD_RIGHT;
        out->stick_rx = NS2_STICK_CENTER;
        out->stick_ry = NS2_STICK_CENTER;
    } else {
        out->buttons &= NS2_BTN_R | NS2_BTN_ZR | NS2_BTN_PLUS | NS2_BTN_HOME |
                        NS2_BTN_A | NS2_BTN_B | NS2_BTN_X | NS2_BTN_Y |
                        NS2_BTN_RSTICK | NS2_BTN_GR;
        out->stick_lx = NS2_STICK_CENTER;
        out->stick_ly = NS2_STICK_CENTER;
    }
    out->nfc_state = 0;
}

/** Report 0x09 电源状态：bit0 外部供电、bit1 充电中、bits2-5 电量等级。 */
static uint8_t power_byte(const ns2_controller_state_t *state)
{
    return (uint8_t)(((state->battery_level & 0x0F) << 2) |
                     ((state->charging ? 1u : 0u) << 1) |
                     (state->external_power ? 1u : 0u));
}

/** Report 0x05 充电状态字节：连接外部供电 0x34，充满 0x20，其余 0。 */
static uint8_t charge_byte(const ns2_controller_state_t *state)
{
    if (state->external_power || state->charging) {
        return 0x34;
    }
    if (state->fully_charged) {
        return 0x20;
    }
    return 0x00;
}

void ns2_pack_stick(uint16_t x, uint16_t y, uint8_t out[3])
{
    out[0] = (uint8_t)(x & 0xFF);
    out[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    out[2] = (uint8_t)((y >> 4) & 0xFF);
}

void ns2_unpack_stick(const uint8_t in[3], uint16_t *x, uint16_t *y)
{
    *x = (uint16_t)(in[0] | ((in[1] & 0x0F) << 8));
    *y = (uint16_t)((in[1] >> 4) | (in[2] << 4));
}

/** Report 0x09 三字节按键位图（controller.md §5.2 Pro Controller 2 表）。 */
static void buttons_09(const ns2_controller_state_t *state, uint8_t out[3])
{
    const uint32_t b = state->buttons;
    out[0] = (uint8_t)(btn_bit(b, NS2_BTN_RSTICK, 7) |
                       btn_bit(b, NS2_BTN_PLUS, 6) |
                       btn_bit(b, NS2_BTN_ZR, 5) |
                       btn_bit(b, NS2_BTN_R, 4) |
                       btn_bit(b, NS2_BTN_X, 3) |
                       btn_bit(b, NS2_BTN_Y, 2) |
                       btn_bit(b, NS2_BTN_A, 1) |
                       btn_bit(b, NS2_BTN_B, 0));
    out[1] = (uint8_t)(btn_bit(b, NS2_BTN_LSTICK, 7) |
                       btn_bit(b, NS2_BTN_MINUS, 6) |
                       btn_bit(b, NS2_BTN_ZL, 5) |
                       btn_bit(b, NS2_BTN_L, 4) |
                       btn_bit(b, NS2_BTN_DPAD_UP, 3) |
                       btn_bit(b, NS2_BTN_DPAD_LEFT, 2) |
                       btn_bit(b, NS2_BTN_DPAD_RIGHT, 1) |
                       btn_bit(b, NS2_BTN_DPAD_DOWN, 0));
    out[2] = (uint8_t)(btn_bit(b, NS2_BTN_C, 4) |
                       btn_bit(b, NS2_BTN_GL, 3) |
                       btn_bit(b, NS2_BTN_GR, 2) |
                       btn_bit(b, NS2_BTN_CAPTURE, 1) |
                       btn_bit(b, NS2_BTN_HOME, 0));
}

/** Report 0x05 四字节按键位图（controller.md §5.1 按键表）。 */
static void buttons_05(const ns2_controller_state_t *state, uint8_t out[4])
{
    const uint32_t b = state->buttons;
    out[0] = (uint8_t)(btn_bit(b, NS2_BTN_ZR, 7) |
                       btn_bit(b, NS2_BTN_R, 6) |
                       btn_bit(b, NS2_BTN_A, 3) |
                       btn_bit(b, NS2_BTN_B, 2) |
                       btn_bit(b, NS2_BTN_X, 1) |
                       btn_bit(b, NS2_BTN_Y, 0));
    out[1] = (uint8_t)(btn_bit(b, NS2_BTN_C, 6) |
                       btn_bit(b, NS2_BTN_CAPTURE, 5) |
                       btn_bit(b, NS2_BTN_HOME, 4) |
                       btn_bit(b, NS2_BTN_LSTICK, 3) |
                       btn_bit(b, NS2_BTN_RSTICK, 2) |
                       btn_bit(b, NS2_BTN_PLUS, 1) |
                       btn_bit(b, NS2_BTN_MINUS, 0));
    out[2] = (uint8_t)(btn_bit(b, NS2_BTN_ZL, 7) |
                       btn_bit(b, NS2_BTN_L, 6) |
                       btn_bit(b, NS2_BTN_DPAD_LEFT, 3) |
                       btn_bit(b, NS2_BTN_DPAD_RIGHT, 2) |
                       btn_bit(b, NS2_BTN_DPAD_UP, 1) |
                       btn_bit(b, NS2_BTN_DPAD_DOWN, 0));
    out[3] = (uint8_t)(btn_bit(b, NS2_BTN_GL, 1) |
                       btn_bit(b, NS2_BTN_GR, 0));
}

void ns2_encode_input_09(uint8_t out[NS2_INPUT_09_LEN],
                         const ns2_controller_state_t *state, uint8_t counter)
{
    memset(out, 0, NS2_INPUT_09_LEN);
    out[0x00] = counter;
    out[0x01] = power_byte(state);
    buttons_09(state, &out[0x02]);
    ns2_pack_stick(state->stick_lx, state->stick_ly, &out[0x05]);
    ns2_pack_stick(state->stick_rx, state->stick_ry, &out[0x08]);
    /* 状态标志：特性位 5（触觉）开启时 0x38，否则 0x30。
     * 0x0C NFC 状态由 amiibo 预置数据驱动（空闲 0x00）；0x0D 耳机状态、
     * 0x0E 运动数据长度本阶段均为 0。 */
    out[0x0B] = state->rumble_enabled ? 0x38 : 0x30;
    out[0x0C] = state->nfc_state;
}

void ns2_encode_input_09_usb(uint8_t out[NS2_INPUT_09_LEN + 1],
                             const ns2_controller_state_t *state, uint8_t counter)
{
    out[0] = NS2_REPORT_ID_09;
    ns2_encode_input_09(&out[1], state, counter);
}

void ns2_encode_input_05(uint8_t out[NS2_INPUT_05_LEN],
                         const ns2_controller_state_t *state, uint32_t counter)
{
    memset(out, 0, NS2_INPUT_05_LEN);
    out[0x00] = (uint8_t)(counter & 0xFF);
    out[0x01] = (uint8_t)((counter >> 8) & 0xFF);
    out[0x02] = (uint8_t)((counter >> 16) & 0xFF);
    out[0x03] = (uint8_t)((counter >> 24) & 0xFF);
    buttons_05(state, &out[0x04]);
    ns2_pack_stick(state->stick_lx, state->stick_ly, &out[0x0A]);
    ns2_pack_stick(state->stick_rx, state->stick_ry, &out[0x0D]);
    /* 鼠标、磁力计、电池电流、IMU 依附的特性位均未启用，保持 0。 */
    out[0x1F] = (uint8_t)(state->battery_mv & 0xFF);
    out[0x20] = (uint8_t)((state->battery_mv >> 8) & 0xFF);
    out[0x21] = charge_byte(state);
    out[0x29] = 0x01;
}

void ns2_encode_input_05_usb(uint8_t out[NS2_INPUT_05_LEN + 1],
                             const ns2_controller_state_t *state, uint32_t counter)
{
    out[0] = NS2_REPORT_ID_05;
    ns2_encode_input_05(&out[1], state, counter);
}

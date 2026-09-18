#include "ns2_report.h"

#include <string.h>

/** 真机抓包（ndeadly/switch2_controller_research 的 btle_procon2_motion_0x000E）
 *  里的一块运动数据：40 字节运动块 + 紧随其后的 8 字节尾段。板卡没有 IMU，
 *  用抓包原值占位比全零更接近真机；块内两处 3 字节小端微秒时间戳（运动块
 *  偏移 0x05 与 0x23）在发送时按上报节奏推进，其余字节保持原值。 */
static const uint8_t s_motion_capture[40 + 8] = {
    0x06, 0x70, 0x95, 0x5B, 0x34, 0xB6, 0x94, 0x78, 0x00, 0x0D,
    0x43, 0xB7, 0xFB, 0x37, 0x42, 0x01, 0x2C, 0x83, 0xFF, 0x41,
    0x34, 0x04, 0x9E, 0x15, 0x0E, 0x88, 0x35, 0x92, 0xCB, 0xCD,
    0x53, 0xC3, 0xA2, 0xBA, 0x49, 0xC3, 0x9F, 0x78, 0x07, 0x51,
    0x6C, 0xBE, 0x81, 0x4B, 0x20, 0x54, 0xDF, 0x58,
};

/** 抓包块内两个时间戳字段的偏移（相对运动块起点）：第二个比第一个晚 2.5ms。 */
#define NS2_MOTION_STAMP_OFFS_A 0x05
#define NS2_MOTION_STAMP_OFFS_B 0x23
#define NS2_REPORT_INTERVAL_US 5000u

/** 把规范化按键位图中的一个键摆到目标字节的指定位上。 */
static uint8_t btn_bit(uint32_t buttons, uint32_t mask, uint8_t shift)
{
    return (uint8_t)(((buttons & mask) != 0) << shift);
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

/** Report 0x09 三字节按键位图（controller.md「专用输入报告」Pro Controller 2 表）。 */
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

/** Report 0x05 四字节按键位图（controller.md「Input Report 0x05」按键表）。 */
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

/** 运动块填充（0x09 报文体；0x05 的 IMU 字段另在 ns2_encode_input_05 里写）。
 *  len_out 是长度字节、data_out 是块首，cap 为块首之后可用字节数（抓包占位块
 *  带 8 字节尾段，空间不够时只填块本体，取用方传 48）。主机开启 IMU 特性位后
 *  长度 0 的报文会被当作不完整输入，因此除 NS2_MOTION_NONE 外用的一档一律填满长度。 */
static void motion_block(uint8_t *len_out, uint8_t *data_out, size_t cap,
                         const ns2_controller_state_t *state, uint8_t counter)
{
    if (state->motion_mode == NS2_MOTION_NONE) {
        *len_out = 0x00;
        return;
    }
    *len_out = NS2_INPUT_09_MOTION_LEN;
    if (state->motion_mode == NS2_MOTION_CAPTURE) {
        const size_t n = cap >= sizeof(s_motion_capture) ? sizeof(s_motion_capture)
                                                         : NS2_INPUT_09_MOTION_LEN;
        memcpy(data_out, s_motion_capture, n);
        const uint32_t base = (uint32_t)counter * NS2_REPORT_INTERVAL_US;
        const uint16_t offs[2] = {NS2_MOTION_STAMP_OFFS_A, NS2_MOTION_STAMP_OFFS_B};
        for (uint8_t i = 0; i < 2; i++) {
            const uint32_t stamp = base + (uint32_t)i * (NS2_REPORT_INTERVAL_US / 2);
            uint8_t *field = &data_out[offs[i]];
            field[0] = (uint8_t)(stamp & 0xFF);
            field[1] = (uint8_t)((stamp >> 8) & 0xFF);
            field[2] = (uint8_t)((stamp >> 16) & 0xFF);
        }
        return;
    }
    /* 实验模式：把输入设备的真实样本按 NS1 的 12 字节样本风格填进块首，
     * 余下字节保持 0。块结构未公开，这一档只为实机 A/B（见 ns2_state.h）。 */
    if (state->motion_mode == NS2_MOTION_SENSOR && state->motion_valid) {
        for (uint8_t s = 0; s < NS2_09_MOTION_SAMPLES; s++) {
            uint8_t *sample = &data_out[s * NS2_09_MOTION_SAMPLE_LEN];
            for (uint8_t axis = 0; axis < 3; axis++) {
                const int16_t gyro = state->gyro[axis];
                const int16_t accel = state->accel[axis];
                sample[axis * 2] = (uint8_t)((uint16_t)gyro & 0xFF);
                sample[axis * 2 + 1] = (uint8_t)((uint16_t)gyro >> 8);
                sample[6 + axis * 2] = (uint8_t)((uint16_t)accel & 0xFF);
                sample[6 + axis * 2 + 1] = (uint8_t)((uint16_t)accel >> 8);
            }
        }
    }
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
    /* 状态标志：特性位 5（触觉）开启时 0x38，否则 0x30（真机抓包：开启触觉
     * 的 0x09 报文该字节恒为 0x38）。0x0C NFC 状态由 amiibo 预置数据驱动
     * （空闲 0x00）；0x0D 耳机状态按输入设备的 3.5mm 状态（或串口 headset
     * 覆盖值）填，未插入为 0x00。 */
    out[0x0B] = state->rumble_enabled ? 0x38 : 0x30;
    out[0x0C] = state->nfc_state;
    out[NS2_09_OFF_HEADSET] = state->headset_state;
    motion_block(&out[NS2_09_OFF_MOTION_LEN], &out[NS2_09_OFF_MOTION],
                 NS2_INPUT_09_LEN - NS2_09_OFF_MOTION, state, counter);
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
    /* 耳机插入位（第 3 字节 bit4）：0x09 侧的耳机状态字节非零即视为已插入。 */
    if (state->headset_state != NS2_HEADSET_NONE) {
        out[0x07] |= NS2_05_BTN3_HEADSET;
    }
    ns2_pack_stick(state->stick_lx, state->stick_ly, &out[0x0A]);
    ns2_pack_stick(state->stick_rx, state->stick_ry, &out[0x0D]);
    /* 鼠标、磁力计与电池电流依附的特性位均未启用，保持 0。 */
    out[0x1F] = (uint8_t)(state->battery_mv & 0xFF);
    out[0x20] = (uint8_t)((state->battery_mv >> 8) & 0xFF);
    out[0x21] = charge_byte(state);
    out[0x29] = 0x01;
    /* IMU 字段（0x2A，18 字节）：时间戳 + 温度 + 加速 XYZ + 陀螺 XYZ。
     * 输入设备带 IMU 时填真值，否则整段保持 0。 */
    if (state->motion_valid) {
        const uint32_t stamp = counter * NS2_REPORT_INTERVAL_US;
        uint8_t *imu = &out[NS2_05_OFF_IMU];
        imu[0] = (uint8_t)(stamp & 0xFF);
        imu[1] = (uint8_t)((stamp >> 8) & 0xFF);
        imu[2] = (uint8_t)((stamp >> 16) & 0xFF);
        imu[3] = (uint8_t)((stamp >> 24) & 0xFF);
        imu[4] = (uint8_t)(NS2_05_IMU_TEMP & 0xFF);
        imu[5] = (uint8_t)(NS2_05_IMU_TEMP >> 8);
        for (uint8_t axis = 0; axis < 3; axis++) {
            const uint16_t accel = (uint16_t)state->accel[axis];
            const uint16_t gyro = (uint16_t)state->gyro[axis];
            imu[6 + axis * 2] = (uint8_t)(accel & 0xFF);
            imu[6 + axis * 2 + 1] = (uint8_t)(accel >> 8);
            imu[12 + axis * 2] = (uint8_t)(gyro & 0xFF);
            imu[12 + axis * 2 + 1] = (uint8_t)(gyro >> 8);
        }
    }
}

void ns2_encode_input_05_usb(uint8_t out[NS2_INPUT_05_LEN + 1],
                             const ns2_controller_state_t *state, uint32_t counter)
{
    out[0] = NS2_REPORT_ID_05;
    ns2_encode_input_05(&out[1], state, counter);
}

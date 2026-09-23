/**
 * NS2 输入报告编码（ns2_report.c）主机端用例：定长黄金报文、按键位表与字段规则三层钉住线格式，
 * 一位错位表现为按键失灵或摇杆偏移、很难靠肉眼定位。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_report.h"
#include "ns2_state.h"

typedef struct {
    uint32_t button;
    const char *name;
    uint8_t offset;
    uint8_t bit;
} button_bit_t;

/** 同时点亮报告里的全部按键字节，便于断言「除该位以外都是 0」。 */
static void encode_09_with_button(uint8_t out[NS2_INPUT_09_LEN], uint32_t button)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.buttons = button;
    ns2_encode_input_09(out, &state, 0);
}

static void encode_05_with_button(uint8_t out[NS2_INPUT_05_LEN], uint32_t button)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.buttons = button;
    ns2_encode_input_05(out, &state, 0);
}

static void idle_report_09(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    uint8_t out[NS2_INPUT_09_LEN];
    ns2_encode_input_09(out, &state, 0x5A);

    uint8_t expected[NS2_INPUT_09_LEN];
    memset(expected, 0, sizeof(expected));
    expected[0x00] = 0x5A; /* 计数器 */
    /* 摇杆中位 2048 = 0x800，按 12 位紧凑打包为 00 08 80。 */
    expected[0x05] = 0x00;
    expected[0x06] = 0x08;
    expected[0x07] = 0x80;
    expected[0x08] = 0x00;
    expected[0x09] = 0x08;
    expected[0x0A] = 0x80;
    expected[0x0B] = 0x30; /* 特性位：未开启触觉 */
    expected[0x0E] = 0x28; /* 运动数据长度：填 40 字节零值占位 */
    CHECK_BYTES(out, expected, sizeof(expected));
}

static void button_bits_09(void)
{
    static const button_bit_t cases[] = {
        {NS2_BTN_A, "A", 0x02, 1},          {NS2_BTN_B, "B", 0x02, 0},
        {NS2_BTN_X, "X", 0x02, 3},          {NS2_BTN_Y, "Y", 0x02, 2},
        {NS2_BTN_R, "R", 0x02, 4},          {NS2_BTN_ZR, "ZR", 0x02, 5},
        {NS2_BTN_PLUS, "PLUS", 0x02, 6},    {NS2_BTN_RSTICK, "RSTICK", 0x02, 7},
        {NS2_BTN_L, "L", 0x03, 4},          {NS2_BTN_ZL, "ZL", 0x03, 5},
        {NS2_BTN_MINUS, "MINUS", 0x03, 6},  {NS2_BTN_LSTICK, "LSTICK", 0x03, 7},
        {NS2_BTN_DPAD_UP, "UP", 0x03, 3},   {NS2_BTN_DPAD_LEFT, "LEFT", 0x03, 2},
        {NS2_BTN_DPAD_RIGHT, "RIGHT", 0x03, 1}, {NS2_BTN_DPAD_DOWN, "DOWN", 0x03, 0},
        {NS2_BTN_C, "C", 0x04, 4},          {NS2_BTN_GL, "GL", 0x04, 3},
        {NS2_BTN_GR, "GR", 0x04, 2},        {NS2_BTN_CAPTURE, "CAPTURE", 0x04, 1},
        {NS2_BTN_HOME, "HOME", 0x04, 0},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        uint8_t out[NS2_INPUT_09_LEN];
        encode_09_with_button(out, cases[index].button);
        for (uint8_t offset = 0x02; offset <= 0x04; offset++) {
            const uint8_t want =
                offset == cases[index].offset ? (uint8_t)(1u << cases[index].bit) : 0x00;
            CHECK_EQ(out[offset], want);
        }
    }
}

static void button_bits_05(void)
{
    static const button_bit_t cases[] = {
        {NS2_BTN_Y, "Y", 0x04, 0},          {NS2_BTN_X, "X", 0x04, 1},
        {NS2_BTN_B, "B", 0x04, 2},          {NS2_BTN_A, "A", 0x04, 3},
        {NS2_BTN_R, "R", 0x04, 6},          {NS2_BTN_ZR, "ZR", 0x04, 7},
        {NS2_BTN_MINUS, "MINUS", 0x05, 0},  {NS2_BTN_PLUS, "PLUS", 0x05, 1},
        {NS2_BTN_RSTICK, "RSTICK", 0x05, 2}, {NS2_BTN_LSTICK, "LSTICK", 0x05, 3},
        {NS2_BTN_HOME, "HOME", 0x05, 4},    {NS2_BTN_CAPTURE, "CAPTURE", 0x05, 5},
        {NS2_BTN_C, "C", 0x05, 6},          {NS2_BTN_DPAD_DOWN, "DOWN", 0x06, 0},
        {NS2_BTN_DPAD_UP, "UP", 0x06, 1},   {NS2_BTN_DPAD_RIGHT, "RIGHT", 0x06, 2},
        {NS2_BTN_DPAD_LEFT, "LEFT", 0x06, 3}, {NS2_BTN_L, "L", 0x06, 6},
        {NS2_BTN_ZL, "ZL", 0x06, 7},        {NS2_BTN_GR, "GR", 0x07, 0},
        {NS2_BTN_GL, "GL", 0x07, 1},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        uint8_t out[NS2_INPUT_05_LEN];
        encode_05_with_button(out, cases[index].button);
        for (uint8_t offset = 0x04; offset <= 0x07; offset++) {
            const uint8_t want =
                offset == cases[index].offset ? (uint8_t)(1u << cases[index].bit) : 0x00;
            CHECK_EQ(out[offset], want);
        }
    }
}

static void stick_packing(void)
{
    static const uint16_t values[] = {0, 1, 0x123, 0x7FF, 0x800, 0xFFF};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
        for (size_t other = 0; other < sizeof(values) / sizeof(values[0]); other++) {
            uint8_t packed[3];
            uint16_t x = 0;
            uint16_t y = 0;
            ns2_pack_stick(values[index], values[other], packed);
            ns2_unpack_stick(packed, &x, &y);
            CHECK_EQ(x, values[index]);
            CHECK_EQ(y, values[other]);
        }
    }

    /* 打包布局：X 低 8 位 + X 高 4 位与 Y 低 4 位同字节 + Y 高 8 位。 */
    uint8_t packed[3];
    ns2_pack_stick(0xABC, 0x123, packed);
    CHECK_EQ(packed[0], 0xBC);
    CHECK_EQ(packed[1], 0x3A);
    CHECK_EQ(packed[2], 0x12);
}


static void power_and_charge_bytes(void)
{
    ns2_controller_state_t state;
    uint8_t out[NS2_INPUT_09_LEN];

    /* 0x09 电源字节：电量 4 位 + 充电中 + 外部供电。 */
    ns2_state_defaults(&state);
    state.battery_level = 9;
    state.charging = true;
    state.external_power = true;
    ns2_encode_input_09(out, &state, 0);
    CHECK_EQ(out[0x01], (uint8_t)((9u << 2) | 0x02u | 0x01u));

    ns2_state_defaults(&state);
    ns2_encode_input_09(out, &state, 0);
    CHECK_EQ(out[0x01], 0x00);

    /* 0x05 充电状态字节：外部供电/充电中 0x34，充满 0x20，其余 0。 */
    ns2_state_defaults(&state);
    state.external_power = true;
    state.fully_charged = true;
    ns2_encode_input_05(out, &state, 0);
    CHECK_EQ(out[0x21], 0x34);

    ns2_state_defaults(&state);
    state.charging = true;
    ns2_encode_input_05(out, &state, 0);
    CHECK_EQ(out[0x21], 0x34);

    ns2_state_defaults(&state);
    state.fully_charged = true;
    ns2_encode_input_05(out, &state, 0);
    CHECK_EQ(out[0x21], 0x20);

    ns2_state_defaults(&state);
    ns2_encode_input_05(out, &state, 0);
    CHECK_EQ(out[0x21], 0x00);

    /* 0x05 尾部固定字段：电池电压小端 + 常量 0x01。 */
    ns2_state_defaults(&state);
    state.battery_mv = 0x1234;
    ns2_encode_input_05(out, &state, 0);
    CHECK_EQ(out[0x1F], 0x34);
    CHECK_EQ(out[0x20], 0x12);
    CHECK_EQ(out[0x29], 0x01);
}

static void feature_flag_and_nfc(void)
{
    uint8_t out[NS2_INPUT_09_LEN];
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    ns2_encode_input_09(out, &state, 0);
    CHECK_EQ(out[0x0B], 0x30); /* 未开启触觉 */
    CHECK_EQ(out[0x0C], 0x00);
    CHECK_EQ(out[0x0D], 0x00); /* 耳机状态 */
    CHECK_EQ(out[0x0E], 0x28); /* 运动数据长度 */

    state.rumble_enabled = true;
    state.nfc_state = 0x05;
    ns2_encode_input_09(out, &state, 0);
    CHECK_EQ(out[0x0B], 0x38);
    CHECK_EQ(out[0x0C], 0x05);
}

/** 主机开启 IMU 特性位（0x0C/0x04 掩码含 bit2）后，0x09 报文必须带上运动
 *  数据块的长度；长度为 0 的报文会被主机当作不完整输入而整体丢弃（表现为
 *  「已连接但按键无反应」）。长度填 40、内容保持全零占位。 */
static void motion_block_is_zero_filled_placeholder(void)
{
    uint8_t out[NS2_INPUT_09_LEN];
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.buttons = NS2_BTN_A;
    ns2_encode_input_09(out, &state, 0);

    CHECK_EQ(out[0x0E], 0x28);
    for (size_t offset = 0x0F; offset < 0x0F + 0x28; offset++) {
        CHECK_EQ(out[offset], 0x00);
    }
    /* 运动块之后只剩保留字节，整包长度不变。 */
    CHECK_EQ(out[0x37], 0x00);
    CHECK_EQ(NS2_INPUT_09_LEN, 63u);
}

static void usb_form_prepends_report_id(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.buttons = NS2_BTN_A | NS2_BTN_ZL;
    state.stick_lx = 0x123;
    state.stick_ry = 0xABC;

    uint8_t ble[NS2_INPUT_09_LEN];
    uint8_t usb[NS2_INPUT_09_LEN + 1];
    ns2_encode_input_09(ble, &state, 0x7F);
    ns2_encode_input_09_usb(usb, &state, 0x7F);
    CHECK_EQ(usb[0], NS2_REPORT_ID_09);
    CHECK_BYTES(&usb[1], ble, sizeof(ble));

    uint8_t ble05[NS2_INPUT_05_LEN];
    uint8_t usb05[NS2_INPUT_05_LEN + 1];
    ns2_encode_input_05(ble05, &state, 0x11223344u);
    ns2_encode_input_05_usb(usb05, &state, 0x11223344u);
    CHECK_EQ(usb05[0], NS2_REPORT_ID_05);
    CHECK_BYTES(&usb05[1], ble05, sizeof(ble05));
}

static void counter_widths(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);

    /* 0x09：计数器是 8 位循环值，高位丢弃。 */
    uint8_t out[NS2_INPUT_09_LEN];
    ns2_encode_input_09(out, &state, 0x1234u & 0xFFu);
    CHECK_EQ(out[0x00], 0x34);

    /* 0x05：计数器是 32 位小端。 */
    uint8_t out05[NS2_INPUT_05_LEN];
    ns2_encode_input_05(out05, &state, 0x12345678u);
    CHECK_EQ(out05[0x00], 0x78);
    CHECK_EQ(out05[0x01], 0x56);
    CHECK_EQ(out05[0x02], 0x34);
    CHECK_EQ(out05[0x03], 0x12);
}

/** 运动块可以整体关掉：主机若本来就不要运动数据，长度 0 是合法形态。 */
static void motion_block_can_be_disabled(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.motion_mode = NS2_MOTION_NONE;

    uint8_t out[NS2_INPUT_09_LEN];
    ns2_encode_input_09(out, &state, 7);
    CHECK_EQ(out[0x0E], 0x00);

    /* 按键/摇杆字段不受运动块开关影响。 */
    state.buttons = NS2_BTN_A;
    ns2_encode_input_09(out, &state, 7);
    CHECK_EQ(out[0x02], 0x02);
    CHECK_EQ(out[0x0E], 0x00);
}

/** 样本占位：长度 40，块内两处时间戳按 5ms 节奏推进，其余字节是样本原值。 */
static void motion_capture_mode_is_monotonic(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.motion_mode = NS2_MOTION_CAPTURE;

    uint8_t first[NS2_INPUT_09_LEN];
    uint8_t second[NS2_INPUT_09_LEN];
    ns2_encode_input_09(first, &state, 10);
    ns2_encode_input_09(second, &state, 11);
    CHECK_EQ(first[0x0E], NS2_INPUT_09_MOTION_LEN);
    CHECK_EQ(second[0x0E], NS2_INPUT_09_MOTION_LEN);

    /* 块内偏移 0x05 与 0x23 各是一个 3 字节小端微秒时间戳，间隔 2.5ms。 */
    const uint32_t a0 = (uint32_t)first[0x14] | ((uint32_t)first[0x15] << 8) |
                        ((uint32_t)first[0x16] << 16);
    const uint32_t b0 = (uint32_t)first[0x32] | ((uint32_t)first[0x33] << 8) |
                        ((uint32_t)first[0x34] << 16);
    const uint32_t a1 = (uint32_t)second[0x14] | ((uint32_t)second[0x15] << 8) |
                        ((uint32_t)second[0x16] << 16);
    CHECK_EQ(a0, 50000u);
    CHECK_EQ(b0 - a0, 2500u);
    CHECK_EQ(a1 - a0, 5000u);

    /* 时间戳之外的字节来自样本：块首 0x06、尾段首字节 0x6C。 */
    CHECK_EQ(first[0x0F], 0x06);
    CHECK_EQ(first[0x13], 0x34);
    CHECK_EQ(first[0x35], 0x07);
    CHECK_EQ(first[0x37], 0x6C);
    CHECK_EQ(first[0x3E], 0x58);

    /* 按键与摇杆字段不受运动块影响。 */
    CHECK_EQ(first[0x02], 0x00);
    CHECK_EQ(first[0x05], 0x00);
    CHECK_EQ(first[0x06], 0x08);
}


HOST_TEST_SUITE(suite_ns2_report, "ns2_report",
                {"静置状态 0x09 整包逐字节", idle_report_09},
                {"0x09 按键位表", button_bits_09},
                {"0x05 按键位表", button_bits_05},
                {"摇杆 12 位打包可逆与布局", stick_packing},
                {"电源与充电状态字节", power_and_charge_bytes},
                {"特性位与 NFC 字段", feature_flag_and_nfc},
                {"运动数据长度非零且块内全零占位", motion_block_is_zero_filled_placeholder},
                {"USB 形态只多一个 Report ID", usb_form_prepends_report_id},
                {"计数器宽度：0x09 8 位 / 0x05 32 位小端", counter_widths},
                {"运动块可整体关闭（长度 0）", motion_block_can_be_disabled},
                {"运动块样本占位的时间戳推进", motion_capture_mode_is_monotonic});

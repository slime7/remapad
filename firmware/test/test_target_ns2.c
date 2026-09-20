/**
 * 转换段（target/ns2）：私有格式到 NS2 报文的映射错了，真机上表现为
 * 「按 A 出了 B」「扳机没反应」或「背键丢失」，这几条正是桥接验收要看的
 * 现象。这里把会话通道换成捕获回调，驱动真实的 ns2_output 编码后断言报文字节，
 * 因此面键位置、背键折并、扳机阈值与电量折叠都在真实编码路径上验证。
 */
#include "host_test.h"

#include <stdio.h>
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

/** 捕获会话的报告格式：0x09 为主，0x05 的耳机插入位另行切换断言。 */
static uint8_t s_capture_format = NS2_REPORT_ID_09;

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
    *report_format = s_capture_format;
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
    /* 耳机状态是跨用例的模块级状态：每个用例都从 auto + 未插入开始。 */
    ns2_output_set_headset_override(false, 0);
    ns2_output_set_headset_derived(NS2_HEADSET_NONE);
    s_capture_format = NS2_REPORT_ID_09;
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
    expect_buttons(PAD_BTN_L1, 0x03, 4);
    expect_buttons(PAD_BTN_R1, 0x02, 4);
    expect_buttons(PAD_BTN_DPAD_UP, 0x03, 3);
    expect_buttons(PAD_BTN_DPAD_DOWN, 0x03, 0);
    expect_buttons(PAD_BTN_DPAD_LEFT, 0x03, 2);
    expect_buttons(PAD_BTN_DPAD_RIGHT, 0x03, 1);
    expect_buttons(PAD_BTN_OPT, 0x02, 6);
    expect_buttons(PAD_BTN_TOUCHPAD, 0x03, 6);
    expect_buttons(PAD_BTN_HOME, 0x04, 0);
    expect_buttons(PAD_BTN_SHARE, 0x04, 1);
    expect_buttons(PAD_BTN_L3, 0x03, 7);
    expect_buttons(PAD_BTN_R3, 0x02, 7);
    /* 静音键只有 PS 的 DualSense 有，目标侧作 C 键。 */
    expect_buttons(PAD_BTN_MUTE, 0x04, 4);
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

/** 串流虚拟手柄（Sunshine/Moonlight）把一颗 View 键双写成 SHARE+触摸板按下
 *  两个 DS 按钮（PC 游戏兼容做法），直译到 NS2 会让一次按键同时点亮减号与
 *  截图。同帧双置按位置语义只出减号。 */
static void share_and_touchpad_in_one_frame_only_minus(void)
{
    prepare();
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.buttons = PAD_BTN_SHARE | PAD_BTN_TOUCHPAD;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x03], 1u << 6); /* 减号亮 */
    CHECK_EQ(s_capture.body[0x04], 0x00);    /* 截图不亮 */
}

static void analog_triggers_digitize_at_half(void)
{
    pad_state_t pad;
    pad_state_defaults(&pad);

    /* 阈值下侧：两位都不亮。 */
    pad.trigger[PAD_TRIGGER_L2] = 2047;
    pad.trigger[PAD_TRIGGER_R2] = 2047;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x02] & 0x20, 0);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0);

    /* 阈值上侧：ZL 与 ZR 同时点亮。 */
    pad.trigger[PAD_TRIGGER_L2] = 2048;
    pad.trigger[PAD_TRIGGER_R2] = 2048;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x02] & 0x20, 0x20);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0x20);

    /* 全按与刚过阈值在报文里没有区别（NS2 只有数字扳机）。 */
    pad.trigger[PAD_TRIGGER_L2] = PAD_AXIS_MAX;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x03] & 0x20, 0x20);

    /* 松开：回到不亮。 */
    pad.trigger[PAD_TRIGGER_L2] = 0;
    pad.trigger[PAD_TRIGGER_R2] = 0;
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

/** 主机在 0x0012 上按 BLE 形态下发 Output Report 0x02：实机写入的载荷是
 *  32 字节（左右各 16 字节 LRA 参数包，不带 Report ID）。把长度判成
 *  33 字节会把每一包震动都丢掉，表现为「主机下发震动，设备毫无反应」，
 *  同时每 20-30 ms 刷一条告警把串口日志淹掉。 */
static void rumble_payload_accepts_ble_form(void)
{
    uint8_t ble[32];
    memset(ble, 0, sizeof(ble));
    ble[0] = 0x40;  /* 左 LRA 状态字 bit6：启用 */
    ble[5] = 0x05;  /* 左路低频振幅 = 20/1023（压 8 位 = 5，越过载波电平）：真的在震 */
    ble[16] = 0x00; /* 右 LRA 状态字：未启用 */

    ns2_rumble_event_t event;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(event.left_on);
    CHECK(!event.right_on);
    CHECK_EQ(event.raw[0], 0x40);
    CHECK_EQ(event.raw[16], 0x00);

    /* 带 Report ID/占位前缀的 33 字节形态：参数包整体后移一字节。 */
    uint8_t with_id[33];
    with_id[0] = 0x00;
    memcpy(&with_id[1], ble, sizeof(ble));
    REQUIRE(ns2_rumble_parse(with_id, sizeof(with_id), &event));
    CHECK(event.left_on);
    CHECK_EQ(event.raw[16], 0x00);

    /* 右路启用且在震、左路关闭：两路状态字分别判定。 */
    ble[0] = 0x00;
    ble[5] = 0x00;
    ble[16] = 0x40;
    ble[21] = 0x05;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(event.right_on);

    /* 过短或空载荷按失败返回，不产生事件。 */
    CHECK(!ns2_rumble_parse(ble, 31, &event));
    CHECK(!ns2_rumble_parse(NULL, sizeof(ble), &event));
    CHECK(!ns2_rumble_parse(ble, sizeof(ble), NULL));
}

/** 主机的震动流是连续包络：低频给冲击、高频给纹理，两颗马达各跟一个频带。
 *  把两带压成单一归一值写进两颗马达，高频纹理会被低频冲掉、手感糊成一片。
 *  LRA 参数包 3 个时序子帧各带 4 个 10 位字段（SDL_hidapi_switch2.c 的打包：
 *  高频频率 bit0-9、高频振幅 bit10-19、低频频率 bit20-29、低频振幅
 *  bit30-39），逐带取三帧最大值、压到 8 位刻度。 */
static void rumble_amplitudes_come_out_per_band(void)
{
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x40; /* 状态字 bit6：启用 */
    /* 子帧 0：高频振幅 10 位 = 400（压 8 位 = 100）。 */
    raw[2] = 0x40;
    raw[3] = 0x06;
    /* 子帧 1：低频振幅 10 位 = 800（压 8 位 = 200）。 */
    raw[10] = 0xC8;

    uint8_t lf = 0;
    uint8_t hf = 0;
    ns2_rumble_band_strengths(raw, &lf, &hf);
    CHECK_EQ(lf, 200);
    CHECK_EQ(hf, 100);
    /* 原有的归一强度 = 两带取大，供「在震」判定与不分带的设备继续使用。 */
    CHECK_EQ(ns2_rumble_strength(raw), 200);

    /* 右路参数包单独解析：高频 10 位 = 400（压 8 位 = 100）、低频 = 20（压
     * 8 位 = 5）。单独给一个 16 字节数组——把 16 字节形参的指针偏到数组外
     * 会让 MSVC 的 RTC 检查误报越界（C4789）。 */
    uint8_t right[16];
    memset(right, 0, sizeof(right));
    right[0] = 0x40;
    right[2] = 0x40; /* 高频 400 -> 位 10-19：字节 2 = 0x40、字节 3 = 0x06 */
    right[3] = 0x06;
    right[5] = 0x05; /* 低频 20 -> 位 30-39：字节 5 = 0x05 */
    ns2_rumble_band_strengths(right, &lf, &hf);
    CHECK_EQ(lf, 5);
    CHECK_EQ(hf, 100);
}

/** LRA 参数包里的 10 位频率字段（高频在位 0-9、低频在位 20-29）：音频触觉
 *  合成按它选驱动频率，这里钉住字段位与「三帧取最大」的语义。 */
static void rumble_frequencies_decode_per_band(void)
{
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x40;
    /* 子帧 0 v = 180 | 55<<20 = 0x370000B4：高频频率 180、低频频率 55。
     * 5 字节小端 -> 字节 1..5 = B4 00 70 03 00。 */
    raw[1] = 0xB4;
    raw[3] = 0x70;
    raw[4] = 0x03;
    /* 子帧 2（字节 11-15）给更大的低频频率 90（位 20-29 落在字节 13/14），
     * 三帧取最大后低频应报 90。 */
    raw[13] = 0xA0;
    raw[14] = 0x05;

    uint16_t lf_hz = 0;
    uint16_t hf_hz = 0;
    ns2_rumble_band_frequencies(raw, &lf_hz, &hf_hz);
    CHECK_EQ(lf_hz, 90);
    CHECK_EQ(hf_hz, 180);

    /* 空包（频率字段全 0）原样报 0：回落缺省值是消费侧（合成）的事。 */
    memset(raw, 0, sizeof(raw));
    ns2_rumble_band_frequencies(raw, &lf_hz, &hf_hz);
    CHECK_EQ(lf_hz, 0);
    CHECK_EQ(hf_hz, 0);
}

/** NS2 的震动是波形描述而不是马达信号：每侧最多 3 个时序子帧（按时间顺序
 *  各播 1/3 周期），每帧一条高频音与一条低频音（频率 + 振幅）。HD 触觉映射
 *  要按它逐帧重整波形，这里把解码逐字段钉住——含 BlueRetro 静止包常量
 *  0x1E100000（低频频率 0x1E1、零振幅）与实机抓包样例 `04 80 01 97 63`。 */
static void rumble_keys_decode_the_full_waveform(void)
{
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x7C; /* 实机抓包状态字：tid 12、有效子帧 3、使能 */
    /* 子帧 0（实机样例 04 80 01 97 63）。 */
    raw[1] = 0x04;
    raw[2] = 0x80;
    raw[3] = 0x01;
    raw[4] = 0x97;
    raw[5] = 0x63;
    /* 子帧 1（BlueRetro 静止包 0x1E100000）：低频频率 0x1E1、零振幅。 */
    raw[8] = 0x10;
    raw[9] = 0x1E;

    ns2_rumble_key_t keys[PAD_RUMBLE_KEY_COUNT];
    const size_t count = ns2_rumble_keys(raw, keys);
    CHECK_EQ(count, 3);
    CHECK_EQ(keys[0].hf_freq, 4);
    CHECK_EQ(keys[0].hf_amp, 96);
    CHECK_EQ(keys[0].lf_freq, 368);
    CHECK_EQ(keys[0].lf_amp, 398);
    CHECK_EQ(keys[1].hf_freq, 0);
    CHECK_EQ(keys[1].hf_amp, 0);
    CHECK_EQ(keys[1].lf_freq, 0x1E1);
    CHECK_EQ(keys[1].lf_amp, 0);
    CHECK_EQ(keys[2].lf_amp, 0);

    /* 载波包的状态字只声明 1 个有效子帧（0x52 的操作数计数 = 1）。 */
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x52;
    raw[8] = 0x10;
    raw[9] = 0x1E;
    CHECK_EQ(ns2_rumble_keys(raw, keys), 1);
    CHECK_EQ(keys[1].lf_freq, 0x1E1); /* 静止包落在子帧 1 的槽位 */

    ns2_rumble_keys(NULL, keys);
    CHECK_EQ(keys[0].hf_amp, 0);
}

/** 游戏里主机会以接近输入上报的频率持续刷「保活包」：状态字使能位为 1、
 *  三帧振幅全 0。真机手柄收到同样的包毫无动静（同一场游戏里实体 JoyCon
 *  不震），把使能位直接当成「在震」转发给输入手柄，就成了一场主机根本没有
 *  的震动。「在震」必须是使能且该路振幅非零。 */
static void zero_amplitude_enable_is_not_rumbling(void)
{
    uint8_t ble[32];
    memset(ble, 0, sizeof(ble));
    ble[0] = 0x40;  /* 左路使能位为 1，但三帧振幅全 0 */
    ble[16] = 0x40; /* 右路同样使能但零幅度 */

    ns2_rumble_event_t event;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);

    /* 左路来一点低频振幅（位 30-39 的 20/1023，压 8 位 = 5，越过载波电平）：
     * 左路在震，零幅度的右路保持安静。 */
    ble[5] = 0x05;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(event.left_on);
    CHECK(!event.right_on);

    /* 使能位撤掉后即使带振幅也不算在震：状态字仍是第一道闸。 */
    ble[0] = 0x00;
    ble[16] = 0x00;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);
}

/** 「查找手柄」页的蜂鸣由 0x0A 采样流承载（0x02 播放 / 0x00 停止），LRA 参数包
 *  只带维持 LRA 通路的载波：BlueRetro 的静止包形态（低频频率 0x1E1、零振幅，
 *  状态字只声明 1 个有效子帧）在位布局记错的年代曾被误读成「高频 1-2/255 的
 *  载波振幅」——按 SDL 打包修正后它就是零振幅，载波天然不算在震。真正的
 *  极低振幅（低于 12/1023）在任何马达上都感知不到，照旧被载波电平闸拦下。 */
static void carrier_level_envelope_is_not_rumbling(void)
{
    uint8_t ble[32];
    memset(ble, 0, sizeof(ble));
    ble[0] = 0x52;  /* 实机载波包状态字：使能位为 1、有效子帧 1 */
    ble[16] = 0x52; /* 右路同样使能 */
    /* 载波段（BlueRetro 静止包）：低频频率 0x1E1、零振幅。 */
    ble[8] = 0x10;
    ble[9] = 0x1E;
    ble[24] = 0x10;
    ble[25] = 0x1E;

    ns2_rumble_event_t event;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);
    uint8_t lf = 1;
    uint8_t hf = 1;
    ns2_rumble_band_strengths(event.raw, &lf, &hf);
    CHECK_EQ(lf, 0);
    CHECK_EQ(hf, 0); /* 频率位不再漏进振幅 */
    uint16_t lf_hz = 0;
    uint16_t hf_hz = 0;
    ns2_rumble_band_frequencies(event.raw, &lf_hz, &hf_hz);
    CHECK_EQ(lf_hz, 0x1E1); /* 载波频率照常解出 */

    /* 真正的极低振幅：10 位 8-11（压 8 位 = 2）不算在震，12-15（压 8 位 = 3，
     * 马达上可感知）才算。 */
    ble[2] = 0x20; /* 高频振幅 10 位 = 8 */
    ble[18] = 0x20;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);

    ble[2] = 0x30; /* 高频振幅 10 位 = 12 */
    ble[18] = 0x30;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(event.left_on);
    CHECK(event.right_on);
}

/** 触觉采样 ID 的解帧（Command 0x0A）：值段第 5 字节是采样 ID（0x02 播放
 *  定位呼叫、0x00 停止），帧过短时退回子命令本身。 */
static void haptic_sample_parse_follows_the_frame(void)
{
    uint8_t sample = 0xFF;
    /* 实机抓包（ns2-search-page.capture）的定位呼叫帧。 */
    static const uint8_t locate[] = {0x0A, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    REQUIRE(ns2_haptic_sample_parse(locate, sizeof(locate), &sample));
    CHECK_EQ(sample, 0x02);

    static const uint8_t stop[] = {0x0A, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(ns2_haptic_sample_parse(stop, sizeof(stop), &sample));
    CHECK_EQ(sample, 0x00);

    /* 非 0x0A 命令不命中。 */
    static const uint8_t other[] = {0x09, 0x91, 0x01, 0x02};
    CHECK(!ns2_haptic_sample_parse(other, sizeof(other), &sample));
}

/** 用 pc/tests/samples/ns2-search-page.capture 回放「查找手柄」页的 20 秒主机
 *  输出：223 条复合输出全部带静置的 LRA 参数包段（搜索页不震），采样流以
 *  约 16 Hz 重发定位呼叫 0x02、收尾用 0x00 停止——发声规则（音色表节奏）
 *  与「载波不算震动」的判据都以此抓包为锚。 */
static void search_page_capture_replays_to_samples_only(void)
{
    FILE *cap = fopen("pc/tests/samples/ns2-search-page.capture", "rb");
    if (cap == NULL) {
        /* 从别的目录跑测试时按仓库相对路径回退。 */
        cap = fopen("../pc/tests/samples/ns2-search-page.capture", "rb");
    }
    REQUIRE(cap != NULL);

    char line[512];
    unsigned total = 0;
    unsigned locate = 0;
    unsigned stop = 0;
    while (fgets(line, sizeof(line), cap) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        /* 行格式：+<秒>s <通道>[0x<h>] seq=<n> <字节>B <十六进制>。回放只
         * 需要「是否 composite」与载荷本体。 */
        const char *hex = strstr(line, "B ");
        REQUIRE(hex != NULL);
        hex += 2;
        uint8_t payload[64];
        size_t len = 0;
        while (len < sizeof(payload)) {
            unsigned byte = 0;
            if (sscanf(hex, "%2x", &byte) != 1) {
                break;
            }
            payload[len++] = (uint8_t)byte;
            hex += 2;
            while (*hex == ' ') {
                hex++;
            }
        }
        REQUIRE(len == 45);
        total++;
        CHECK_EQ(payload[0], 0x00); /* 复合输出的填充字节 */

        /* 震动段（字节 1-32）：静置零包，不算在震。 */
        ns2_rumble_event_t rumble;
        REQUIRE(ns2_rumble_parse(&payload[1], 32, &rumble));
        CHECK(!rumble.left_on);
        CHECK(!rumble.right_on);

        /* 命令帧（字节 33 起）：采样流。 */
        uint8_t sample = 0xFF;
        REQUIRE(ns2_haptic_sample_parse(&payload[33], len - 33, &sample));
        if (sample == 0x02) {
            locate++;
        } else if (sample == 0x00) {
            stop++;
        }
    }
    fclose(cap);
    CHECK_EQ(total, 223);
    CHECK_EQ(locate, 207);
    CHECK_EQ(stop, 16);
}

/**
 * 3.5mm 耳机状态：能力位声明了耳机字段才当真，插入即派生出 0x09 的 0x0D =
 * 0x05；未声明时保持「未插入」——兜底布局的随机字节不能冒充耳机状态。
 * 带麦一档（0x07 / 0x0F）会被主机拒绝（换上后约 150 ms 掉订阅），因此派生
 * 值不上报带麦位，私有格式里的 headset_mic 仍然照常解析。
 */
static void headset_state_follows_the_input_device(void)
{
    prepare();
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.caps = PAD_CAP_MIC;

    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], NS2_HEADSET_NONE);

    pad.headset_present = true;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], NS2_HEADSET_STEREO);

    pad.headset_mic = true;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], NS2_HEADSET_STEREO);

    pad.caps = 0;
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], NS2_HEADSET_NONE);
}

/** 0x05 会话同样带耳机插入位（第 3 字节 bit4），与 0x09 的 0x0D 同一个来源。 */
static void headset_bit_rides_report_05_too(void)
{
    prepare();
    s_capture_format = NS2_REPORT_ID_05;
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.caps = PAD_CAP_MIC;
    pad.headset_present = true;

    target_send_pad(&pad);
    CHECK_EQ(s_capture.report_id, NS2_REPORT_ID_05);
    CHECK_EQ(s_capture.body[0x07], NS2_05_BTN3_HEADSET);
}

/** 串口 headset 覆盖值：0x09 的 0x0D 跟覆盖值走，auto 回来即恢复派生值。 */
static void headset_override_pins_the_reported_byte(void)
{
    prepare();
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.caps = PAD_CAP_MIC;
    pad.headset_present = true;

    ns2_output_set_headset_override(true, 0x0D);
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], 0x0D);

    ns2_output_set_headset_override(false, 0);
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[NS2_09_OFF_HEADSET], NS2_HEADSET_STEREO);

    uint8_t value = 0xFF;
    CHECK(!ns2_output_headset_override(&value));
    CHECK_EQ(value, 0x00);
    CHECK_EQ(ns2_output_headset_byte(), ns2_output_headset_derived());
}

/**
 * 电量来源跟输入设备走：自报电量（PAD_CAP_BATTERY 且本帧解出电量字段）的
 * 手柄把真实电量折进电源字节，主机看到的就是手柄电量；设备没带电量数据
 * （Xbox 系没有电量字节、报告过短、桥上没插手柄）时保持板载电池兜底。
 * 0x05 报文专用的端电压字段按电压—容量表反演成名义值，不泄漏板载电压。
 */
static void pad_battery_overrides_board_battery(void)
{
    prepare();
    const pad_target_facts_t board = {
        .battery_level = 7,
        .battery_mv = 3786,
        .charging = true,
        .external_power = true,
    };
    pad_target_facts_t facts = board;
    target_set_facts(&facts);

    pad_state_t pad;
    pad_state_defaults(&pad);
    target_send_pad(&pad);
    /* 手柄没有自报电量：电源字节保持板载档位（7 档 + 充电 + 外部供电）。 */
    CHECK_EQ(s_capture.body[0x01], (uint8_t)((7u << 2) | 0x02u | 0x01u));

    /* 手柄自报 50%（0x09 的 5 档）且未充电：电源字节跟手柄走，端电压按
     * 电压—容量表反演成名义值（50% 恰为表点 3605mV），不再泄漏板载电压。 */
    pad.caps = PAD_CAP_BATTERY;
    pad.battery_percent = 50;
    pad.battery_present = true;
    facts = board;
    target_apply_pad_battery(&facts, &pad);
    CHECK_EQ(facts.battery_level, 5);
    CHECK_EQ(facts.battery_mv, 3605);
    target_set_facts(&facts);
    target_send_pad(&pad);
    CHECK_EQ(s_capture.body[0x01], (uint8_t)(5u << 2));

    /* 能力位置了但这帧没解出电量（报告过短等）：继续用板载值。 */
    pad_state_t partial;
    pad_state_defaults(&partial);
    partial.caps = PAD_CAP_BATTERY;
    facts = board;
    target_apply_pad_battery(&facts, &partial);
    CHECK_EQ(facts.battery_level, 7);
    CHECK(facts.charging);
    CHECK(facts.external_power);
}

HOST_TEST_SUITE(suite_target_ns2, "target_ns2",
                {"面键按位置映射到 NS2 的 A/B/X/Y（私有用 PS 键名）",
                 face_buttons_keep_position_semantics},
                {"肩键、方向键、选择类与系统键", shoulders_dpad_and_system_keys},
                {"四颗背键按侧折进 GL / GR", back_buttons_fold_into_gl_and_gr},
                {"SHARE 与触摸板同帧双置只出减号（串流一颗键双写）",
                 share_and_touchpad_in_one_frame_only_minus},
                {"扳机按 50% 阈值数字化成 ZL / ZR", analog_triggers_digitize_at_half},
                {"摇杆原样进报文且中位正确", sticks_keep_values_and_center},
                {"目标事实折进电量字节", target_facts_fold_into_power_byte},
                {"上报主机的电量跟输入设备自报值走，板载电池兜底",
                 pad_battery_overrides_board_battery},
                {"NS2 吃不下能力位也不改报文", unconsumed_caps_do_not_change_the_report},
                {"未识别型号兜底后仍照常上报", unknown_model_still_reports_keys},
                {"震动载荷接受 BLE 形态的 32 字节", rumble_payload_accepts_ble_form},
                {"LRA 参数包按低频/高频频带分别给出振幅", rumble_amplitudes_come_out_per_band},
                {"LRA 参数包按频带解出 10 位驱动频率", rumble_frequencies_decode_per_band},
                {"LRA 参数包逐帧解出完整波形（含静止包与实机样例）",
                 rumble_keys_decode_the_full_waveform},
                {"零幅度的使能保活包不算在震", zero_amplitude_enable_is_not_rumbling},
                {"查找手柄页的载波不算在震（频率位不再漏进振幅）",
                 carrier_level_envelope_is_not_rumbling},
                {"触觉采样 ID 按命令帧解出", haptic_sample_parse_follows_the_frame},
                {"搜索页抓包回放：只发采样流、震动段全程静置",
                 search_page_capture_replays_to_samples_only},
                {"耳机状态按输入设备的 3.5mm 状态派生（0x09 的 0x0D，只报插入）",
                 headset_state_follows_the_input_device},
                {"0x05 报告带耳机插入位", headset_bit_rides_report_05_too},
                {"串口 headset 覆盖值钉住上报的耳机字节",
                 headset_override_pins_the_reported_byte});

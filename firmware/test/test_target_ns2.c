/**
 * 转换段（target/ns2）：私有格式到 NS2 报文的映射错了，真机上表现为
 * 「按 A 出了 B」「扳机没反应」或「背键丢失」，这几条正是桥接验收要看的
 * 现象。这里把会话通道换成捕获回调，驱动真实的 ns2_output 编码后断言报文字节，
 * 因此面键位置、背键折并、扳机阈值与电量折叠都在真实编码路径上验证。
 */
#include "host_test.h"

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
    ble[2] = 0x20;  /* 左路低频振幅非零（归一 4，越过载波电平）：真的在震 */
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
    ble[2] = 0x00;
    ble[16] = 0x40;
    ble[18] = 0x20;
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
 *  LRA 参数包三组操作数据各带低频 10 位与高频 8 位振幅，逐带取三组最大值、
 *  低频压到 8 位刻度（5 字节小端 v：低频振幅在 bit9 起、高频在 bit28 起）。 */
static void rumble_amplitudes_come_out_per_band(void)
{
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x40; /* 状态字 bit6：启用 */
    /* 第 0 组：低频 10 位 = 400（压 8 位 = 100）、高频 = 20。
     * v = 400<<9 | 20<<28 = 0x1_4003_2000。 */
    raw[1] = 0x00;
    raw[2] = 0x20;
    raw[3] = 0x03;
    raw[4] = 0x40;
    raw[5] = 0x01;
    /* 第 1 组：高频 = 50（v = 50<<28 = 0x3_2000_0000），三组取最大后高频应为
     * 50。 */
    raw[9] = 0x20;
    raw[10] = 0x03;

    uint8_t lf = 0;
    uint8_t hf = 0;
    ns2_rumble_band_strengths(raw, &lf, &hf);
    CHECK_EQ(lf, 100);
    CHECK_EQ(hf, 50);
    /* 原有的归一强度 = 两带取大，供「在震」判定与不分带的设备继续使用。 */
    CHECK_EQ(ns2_rumble_strength(raw), 100);

    /* 右路参数包单独解析：低频 10 位 = 800（压 8 位 = 200）。单独给一个
     * 16 字节数组——把 16 字节形参的指针偏到数组外会让 MSVC 的 RTC 检查
     * 误报越界（C4789）。 */
    uint8_t right[16];
    memset(right, 0, sizeof(right));
    right[0] = 0x40;
    right[2] = 0x40; /* v = 800<<9 = 0x64000 -> 字节 1 = 0x40、字节 2 = 0x06 */
    right[3] = 0x06;
    ns2_rumble_band_strengths(right, &lf, &hf);
    CHECK_EQ(lf, 200);
    CHECK_EQ(hf, 0);
}

/** 游戏里主机会以接近输入上报的频率持续刷「保活包」：状态字使能位为 1、
 *  三组振幅全 0。真机手柄收到同样的包毫无动静（同一场游戏里实体 JoyCon
 *  不震），把使能位直接当成「在震」转发给输入手柄，就成了一场主机根本没有
 *  的震动。「在震」必须是使能且该路振幅非零。 */
static void zero_amplitude_enable_is_not_rumbling(void)
{
    uint8_t ble[32];
    memset(ble, 0, sizeof(ble));
    ble[0] = 0x40;  /* 左路使能位为 1，但三组振幅全 0 */
    ble[16] = 0x40; /* 右路同样使能但零幅度 */

    ns2_rumble_event_t event;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);

    /* 左路来一点低频振幅（字节 2 的 0x20，8 位刻度下归一为 4，越过载波电平）：
     * 左路在震，零幅度的右路保持安静。 */
    ble[2] = 0x20;
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
 *  只带载波电平：状态字使能、低频全 0、高频振幅 1-2/255（2026-09-18 实机：反馈
 *  帧高频 1/1）。这种电平在任何马达上都感知不到，真机手柄同样不震——若判成
 *  「在震」，采样退化成的短震动会被「主机已经在震时不动」的闸门压住，查找
 *  手柄页点击手柄就毫无动静。 */
static void carrier_level_envelope_is_not_rumbling(void)
{
    uint8_t ble[32];
    memset(ble, 0, sizeof(ble));
    ble[0] = 0x52; /* 实机载波包状态字：使能位为 1 */
    ble[4] = 0x10; /* 第 0 组高频振幅 = 1（1<<28 落在字节 4 的高半） */
    ble[16] = 0x52;
    ble[20] = 0x10;

    ns2_rumble_event_t event;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);
    uint8_t lf = 1;
    uint8_t hf = 1;
    ns2_rumble_band_strengths(event.raw, &lf, &hf);
    CHECK_EQ(lf, 0);
    CHECK_EQ(hf, 1); /* 强度照常解出：只是不当作在震 */

    /* 载波上限（2/255）之内都不算在震。 */
    ble[4] = 0x20;
    ble[20] = 0x20;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(!event.left_on);
    CHECK(!event.right_on);

    /* 到 3/255（马达上可感知）才算在震。 */
    ble[4] = 0x30;
    ble[20] = 0x30;
    REQUIRE(ns2_rumble_parse(ble, sizeof(ble), &event));
    CHECK(event.left_on);
    CHECK(event.right_on);
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
                {"零幅度的使能保活包不算在震", zero_amplitude_enable_is_not_rumbling},
                {"查找手柄页的载波电平不算在震（高频 1-2/255）",
                 carrier_level_envelope_is_not_rumbling},
                {"耳机状态按输入设备的 3.5mm 状态派生（0x09 的 0x0D，只报插入）",
                 headset_state_follows_the_input_device},
                {"0x05 报告带耳机插入位", headset_bit_rides_report_05_too},
                {"串口 headset 覆盖值钉住上报的耳机字节",
                 headset_override_pins_the_reported_byte});

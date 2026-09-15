/**
 * 反馈编码（pad/feedback.c）：主机下发的震动 / 玩家灯 / 触觉采样按布局行
 * 编码成对应手柄的输出报告。偏移取自公开实现（本轮没有实机核对），这里把
 * 每个家族的字节布局钉住——实机对比时只要报告字节一致就说明表没填错。
 *
 * 同代透传单独一例：NS2 手柄直接吃主机的 LRA 参数包，不做任何字段映射。
 */
#include "host_test.h"

#include <string.h>

#include "feedback.h"
#include "pad_state.h"

static pad_feedback_t feedback_default(void)
{
    pad_feedback_t feedback;
    pad_feedback_defaults(&feedback);
    return feedback;
}

/** 蓝牙输出报告尾部 CRC32 的黄金值（算法与来源见 dualsense 蓝牙用例）：
 *  依次对应「左 255 / 右 128 / 1P」「停止震动 / 1P」「无震动 2P」「DS4 左 64 / 2P」。 */
static const uint8_t s_crc_ds5_rumble[4] = {0xcb, 0x4c, 0x00, 0x6d};
static const uint8_t s_crc_ds5_stop[4] = {0x33, 0xa9, 0x65, 0xb1};
static const uint8_t s_crc_ds5_2p[4] = {0xff, 0x75, 0x4c, 0x6b};
static const uint8_t s_crc_ds4_bt[4] = {0xbc, 0xb2, 0x30, 0x41};

static void dualsense_usb_encodes_rumble_and_led(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 255;
    feedback.rumble_on[PAD_TRIGGER_R2] = true;
    feedback.rumble_strength[PAD_TRIGGER_R2] = 128;
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 48);
    CHECK_EQ(out[0], 0x02); /* 报告 ID */
    CHECK_EQ(out[1], 0x03); /* valid_flag0：兼容震动 + 关音频触觉 */
    CHECK_EQ(out[2], 0x14); /* valid_flag1：灯条 + 玩家指示灯 */
    CHECK_EQ(out[3], 128);  /* 右小马达 */
    CHECK_EQ(out[4], 255);  /* 左大马达 */
    CHECK_EQ(out[39], 0x02); /* valid_flag2：灯条设置控制 */
    CHECK_EQ(out[42], 0x02); /* 灯条设置值：淡出 */
    CHECK_EQ(out[44], 0x04); /* 1P 灯位：只有中间一颗 */
    CHECK_EQ(out[45], 0x00); /* 灯条蓝（bit0） */
    CHECK_EQ(out[46], 0x00);
    CHECK_EQ(out[47], 0xFF);

    /* 震动关掉后强度写 0，其余字段照发。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = false;
    feedback.rumble_on[PAD_TRIGGER_R2] = false;
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)),
             48);
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[4], 0);
}

static void ds4_usb_encodes_rumble_and_lightbar(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 64;
    feedback.player_led = 0x02; /* bit1 → 红 */

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 32);
    CHECK_EQ(out[0], 0x05); /* 报告 ID */
    CHECK_EQ(out[1], 0x03); /* flags：震动 + 灯条颜色 */
    CHECK_EQ(out[4], 0);    /* 右小马达（未震） */
    CHECK_EQ(out[5], 64);   /* 左大马达 */
    CHECK_EQ(out[6], 0xFF); /* 灯条 R */
    CHECK_EQ(out[7], 0x00);
    CHECK_EQ(out[8], 0x00);
}

/** 蓝牙形态：b1 是序号/标签字节、b2 是固定魔数 0x10、公共段从 b3 起，末 4 字节
 *  是 CRC32。黄金字节按 Linux hid-playstation.c 的算法算得（种子字节 0xA2 先过
 *  一遍、结果小端写末 4 字节）——实机验证过：缺了这段 CRC，主机整份报告都不认，
 *  写回成功而手柄毫无反应。 */
static void dualsense_bt_encodes_framed_report(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 255;
    feedback.rumble_on[PAD_TRIGGER_R2] = true;
    feedback.rumble_strength[PAD_TRIGGER_R2] = 128;
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 78);
    CHECK_EQ(out[0], 0x31); /* 蓝牙报告 ID */
    CHECK_EQ(out[1], 0x00); /* 序号与标签半字节 */
    CHECK_EQ(out[2], 0x10); /* 固定魔数 */
    CHECK_EQ(out[3], 0x03);
    CHECK_EQ(out[4], 0x14);
    CHECK_EQ(out[5], 128); /* 右小马达 */
    CHECK_EQ(out[6], 255); /* 左大马达 */
    CHECK_EQ(out[41], 0x02); /* valid_flag2：灯条设置控制 */
    CHECK_EQ(out[44], 0x02); /* 灯条设置值：淡出（主机连接动画会一直盖着灯） */
    CHECK_EQ(out[46], 0x04); /* 1P 灯位 */
    CHECK_EQ(out[47], 0x00); /* 灯条蓝 */
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0xFF);
    CHECK_BYTES(&out[74], s_crc_ds5_rumble, sizeof(s_crc_ds5_rumble));

    /* 停止震动：马达清零，CRC 跟着报告体一起变。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = false;
    feedback.rumble_on[PAD_TRIGGER_R2] = false;
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[5], 0);
    CHECK_EQ(out[6], 0);
    CHECK_BYTES(&out[74], s_crc_ds5_stop, sizeof(s_crc_ds5_stop));
}

/** 玩家指示灯按设备自己的灯位模式点亮：DualSense 的五颗灯是一组固定模式，
 *  直写主机掩码会点错灯（2P 该是中间加外两颗，不是 bit1）。 */
static void dualsense_player_led_follows_pattern(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.player_led = 0x02;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[46], 0x0A);
    CHECK_EQ(out[47], 0xFF); /* 2P 灯条红 */
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0x00);
    CHECK_BYTES(&out[74], s_crc_ds5_2p, sizeof(s_crc_ds5_2p));

    /* 没有分配玩家号时五颗全灭。 */
    feedback.player_led = 0x00;
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out));
    CHECK_EQ(out[46], 0x00);
}

/** DS4 蓝牙同样带 hw_control 头与尾部 CRC32：公共段从 b3 起，灯条在 b8-b10。 */
static void ds4_bt_encodes_framed_report(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 64;
    feedback.player_led = 0x02;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x09CC, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 78);
    CHECK_EQ(out[0], 0x11); /* 蓝牙报告 ID */
    CHECK_EQ(out[1], 0xC0); /* hw_control：HID + CRC32 */
    CHECK_EQ(out[3], 0x03); /* flags：马达 + 灯条 */
    CHECK_EQ(out[6], 0);    /* 右小马达 */
    CHECK_EQ(out[7], 64);   /* 左大马达 */
    CHECK_EQ(out[8], 0xFF); /* 灯条红 */
    CHECK_EQ(out[9], 0x00);
    CHECK_EQ(out[10], 0x00);
    CHECK_BYTES(&out[74], s_crc_ds4_bt, sizeof(s_crc_ds4_bt));
}

/** 主机反馈是持续状态：玩家灯事件之后的震动事件不能把灯写灭，触觉采样则只
 *  在带它的事件里有效（主机用采样 0x00 收掉提示音后马达要停）。 */
static void held_feedback_keeps_steady_state(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    uint8_t out[PAD_OUTPUT_MAX];

    event.player_led = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_PLAYER_LED, &event);
    CHECK_EQ(held.player_led, 0x02);

    /* 采样事件当次出脉冲，下一次事件把它清掉。 */
    event = feedback_default();
    event.haptic_sample_valid = true;
    event.haptic_sample = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);
    CHECK(held.haptic_sample_valid);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0xC0);

    event = feedback_default();
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK(!held.haptic_sample_valid);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0x00);
    CHECK_EQ(out[46], 0x0A); /* 玩家灯仍然保留 */

    /* 只有震动的后续事件：玩家灯保留，震动字段按事件覆盖。 */
    event = feedback_default();
    event.rumble_on[PAD_TRIGGER_L2] = true;
    event.rumble_strength[PAD_TRIGGER_L2] = 64;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK_EQ(held.player_led, 0x02);
    CHECK_EQ(held.rumble_strength[PAD_TRIGGER_L2], 64);

    /* 编码出来的帧里玩家灯还在：灯不会被随后的震动帧写灭。 */
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out)), 78);
    CHECK_EQ(out[46], 0x0A);
    CHECK_EQ(out[6], 64);
}

static void ns2_pad_relays_lra_payload_verbatim(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_raw[PAD_TRIGGER_L2][0] = 0x7F;
    feedback.rumble_raw[PAD_TRIGGER_R2][0] = 0x3F;
    feedback.rumble_raw[PAD_TRIGGER_R2][15] = 0x99;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x057E, 0x2069, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 42);
    CHECK_EQ(out[0], 0x02);
    CHECK_EQ(out[1], 0x7F);
    CHECK_EQ(out[17], 0x3F);
    CHECK_EQ(out[32], 0x99);
    CHECK_EQ(out[41], 0x00);
}

static void haptic_sample_degrades_to_short_pulse(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.haptic_sample_valid = true;
    feedback.haptic_sample = 0x05;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out)),
             32);
    CHECK_EQ(out[4], 0xC0); /* 右小马达脉冲 */
    CHECK_EQ(out[5], 0xC0);

    /* 主机已经在震时采样不叠加。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 32;
    pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out));
    CHECK_EQ(out[5], 32);
    CHECK_EQ(out[4], 0x00); /* 主机已经在震：采样不叠加到另一侧 */
}

/** 采样 ID 0x00 是「静音 / 停止播放」（controller.md §6.2），不是一次播放：
 *  主机用它收掉「寻找手柄」的提示音时，马达必须停，不能停在脉冲值上。 */
static void haptic_stop_sample_silences_motors(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.haptic_sample_valid = true;
    feedback.haptic_sample = 0x00;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out)),
             32);
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x00);
}

static void ns1_rumble_uses_band_template(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 255;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x057E, 0x2009, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 9);
    CHECK_EQ(out[0], 0x10); /* 报告 ID */
    CHECK_EQ(out[1], 0x00); /* 低频段固定头 */
    CHECK_EQ(out[2], 0x01);
    CHECK_EQ(out[3], 0x40); /* 高频段满量程 */
    CHECK_EQ(out[4], 0x40); /* 左马达振幅：按 0x40 满量程缩放 */
    CHECK_EQ(out[8], 0x00); /* 右马达未震 */
}

static void unknown_device_has_no_feedback_channel(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 100;
    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x1234, 0x5678, &feedback, out, sizeof(out)),
             0);
    CHECK(pad_feedback_last_layout() == NULL);
}

HOST_TEST_SUITE(suite_pad_feedback, "pad_feedback",
                {"DualSense 有线的震动、玩家灯与灯条编码", dualsense_usb_encodes_rumble_and_led},
                {"DS4 有线的震动与灯条颜色编码", ds4_usb_encodes_rumble_and_lightbar},
                {"DualSense 蓝牙的震动写回带上帧头与 CRC32", dualsense_bt_encodes_framed_report},
                {"玩家指示灯按设备灯位模式点亮", dualsense_player_led_follows_pattern},
                {"DS4 蓝牙的震动写回带上帧头与 CRC32", ds4_bt_encodes_framed_report},
                {"NS2 手柄原样接收主机的 LRA 参数包", ns2_pad_relays_lra_payload_verbatim},
                {"触觉采样在无采样能力的设备上退化成短震动",
                 haptic_sample_degrades_to_short_pulse},
                {"停止播放的采样不会把马达留在脉冲上", haptic_stop_sample_silences_motors},
                {"持续帧保留玩家灯、采样只在当次事件生效", held_feedback_keeps_steady_state},
                {"NS1 的震动按固定头加振幅写入", ns1_rumble_uses_band_template},
                {"未识别设备没有反馈通道", unknown_device_has_no_feedback_channel});

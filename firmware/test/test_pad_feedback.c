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
    CHECK_EQ(len, 50);
    CHECK_EQ(out[0], 0x02); /* 报告 ID */
    CHECK_EQ(out[1], 0x01); /* valid_flag0：震动 */
    CHECK_EQ(out[2], 0x14); /* valid_flag1：灯条 + 玩家灯 */
    CHECK_EQ(out[3], 128);  /* 右小马达 */
    CHECK_EQ(out[4], 255);  /* 左大马达 */
    CHECK_EQ(out[46], 0x01); /* 玩家灯掩码 */
    CHECK_EQ(out[47], 0x00); /* 灯条蓝（bit0） */
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0xFF);

    /* 震动关掉后强度写 0，其余字段照发。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = false;
    feedback.rumble_on[PAD_TRIGGER_R2] = false;
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)),
             50);
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

static void dualsense_bt_offsets_shift_by_one(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_R2] = true;
    feedback.rumble_strength[PAD_TRIGGER_R2] = 200;
    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 78);
    CHECK_EQ(out[0], 0x31); /* 蓝牙报告 ID */
    CHECK_EQ(out[2], 0x01);
    CHECK_EQ(out[3], 0x14);
    CHECK_EQ(out[4], 200);
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
                {"DualSense 蓝牙的输出报告整体后移一位", dualsense_bt_offsets_shift_by_one},
                {"NS2 手柄原样接收主机的 LRA 参数包", ns2_pad_relays_lra_payload_verbatim},
                {"触觉采样在无采样能力的设备上退化成短震动",
                 haptic_sample_degrades_to_short_pulse},
                {"NS1 的震动按固定头加振幅写入", ns1_rumble_uses_band_template},
                {"未识别设备没有反馈通道", unknown_device_has_no_feedback_channel});

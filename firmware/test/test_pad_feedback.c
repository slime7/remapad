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

/** 数据面的渲染步骤：主机只重发采样 ID，编码吃的采样字节是该采样音色在
 *  当前时刻的幅度（dp_plane 每步替换后再编码）。 */
static void render_haptic_envelope(pad_feedback_t *held, uint32_t age_ms)
{
    if (held->haptic_sample_valid && held->haptic_sample != 0) {
        held->haptic_sample = pad_haptic_pulse_envelope(held->haptic_sample, age_ms);
    }
}

/** 蓝牙输出报告尾部 CRC32 的黄金值（算法与来源见 dualsense 蓝牙用例）：
 *  依次对应「左 255 / 右 128 / 1P」「停止震动 / 1P」「无震动 2P」「DS4 左 64 / 2P」。
 *  DS5 的三组是灯条退出反馈通道后的取值（valid_flag1 只置玩家灯、灯条设置与
 *  RGB 字节全零，2026-09-18 重算）。 */
static const uint8_t s_crc_ds5_rumble[4] = {0x37, 0xe0, 0xa8, 0xda};
static const uint8_t s_crc_ds5_stop[4] = {0xcf, 0x05, 0xcd, 0x06};
static const uint8_t s_crc_ds5_2p[4] = {0x69, 0xe3, 0xde, 0x0e};
static const uint8_t s_crc_ds4_bt[4] = {0xbc, 0xb2, 0x30, 0x41};

static void dualsense_usb_encodes_rumble_and_led(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 255;
    feedback.rumble_on[PAD_TRIGGER_R2] = true;
    feedback.rumble_strength[PAD_TRIGGER_R2] = 128;
    /* 小马达跟高频带：右路的高频振幅单独给。 */
    feedback.rumble_hf_strength[PAD_TRIGGER_R2] = 128;
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 48);
    CHECK_EQ(out[0], 0x02); /* 报告 ID */
    CHECK_EQ(out[1], 0x03); /* valid_flag0：兼容震动 + 关音频触觉 */
    CHECK_EQ(out[2], 0x10); /* valid_flag1：只置玩家指示灯，灯条不声明有效 */
    CHECK_EQ(out[3], 128);  /* 右小马达 */
    CHECK_EQ(out[4], 255);  /* 左大马达 */
    CHECK_EQ(out[39], 0x00); /* 不写灯条设置控制 */
    CHECK_EQ(out[42], 0x00); /* 不写灯条设置值 */
    CHECK_EQ(out[44], 0x04); /* 1P 灯位：只有中间一颗 */
    CHECK_EQ(out[45], 0x00); /* 灯条 RGB 不声明有效，手柄保持自己的颜色 */
    CHECK_EQ(out[46], 0x00);
    CHECK_EQ(out[47], 0x00);

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
    /* 小马达跟高频带：右路的高频振幅单独给。 */
    feedback.rumble_hf_strength[PAD_TRIGGER_R2] = 128;
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    /* 黄金 CRC 按序号 0 的报告体复算，先回零。 */
    pad_feedback_bt_seq_reset();
    const size_t len = pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out,
                                           sizeof(out));
    CHECK_EQ(len, 78);
    CHECK_EQ(out[0], 0x31); /* 蓝牙报告 ID */
    CHECK_EQ(out[1], 0x00); /* 序号与标签半字节 */
    CHECK_EQ(out[2], 0x10); /* 固定魔数 */
    CHECK_EQ(out[3], 0x03);
    CHECK_EQ(out[4], 0x10); /* 只置玩家指示灯 */
    CHECK_EQ(out[5], 128); /* 右小马达 */
    CHECK_EQ(out[6], 255); /* 左大马达 */
    CHECK_EQ(out[41], 0x00); /* 不写灯条设置控制 */
    CHECK_EQ(out[44], 0x00); /* 不写灯条设置值 */
    CHECK_EQ(out[46], 0x04); /* 1P 灯位 */
    CHECK_EQ(out[47], 0x00); /* 灯条 RGB 不声明有效 */
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0x00);
    CHECK_BYTES(&out[74], s_crc_ds5_rumble, sizeof(s_crc_ds5_rumble));

    /* 停止震动：马达清零，CRC 跟着报告体一起变。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = false;
    feedback.rumble_on[PAD_TRIGGER_R2] = false;
    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[5], 0);
    CHECK_EQ(out[6], 0);
    CHECK_BYTES(&out[74], s_crc_ds5_stop, sizeof(s_crc_ds5_stop));
}

/** 主机的震动流是连续包络（低频给出冲击、高频给出纹理）：DualSense 的两颗
 *  马达各跟一个频带——大马达跟低频、小马达跟高频，而不是把同一个归一值
 *  写进两颗马达。只来高频纹理时大马达必须不动。 */
static void dualsense_motors_follow_rumble_bands(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 200;    /* 左路低频带振幅 */
    feedback.rumble_on[PAD_TRIGGER_R2] = true;
    feedback.rumble_hf_strength[PAD_TRIGGER_R2] = 40;  /* 右路高频带振幅 */

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)), 48);
    CHECK_EQ(out[4], 200); /* b4 左大马达：跟低频带 */
    CHECK_EQ(out[3], 40);  /* b3 右小马达：跟高频带 */

    /* 低频撤掉、只剩高频纹理时大马达不动。 */
    feedback.rumble_strength[PAD_TRIGGER_L2] = 0;
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)), 48);
    CHECK_EQ(out[4], 0);
    CHECK_EQ(out[3], 40);
}

/** 监听者按两带解出强度后要经持续帧合并（pad_feedback_apply）才到编码——
 *  合并漏拷高频带会把高频纹理编码成全零马达字节，写回层按「字节没变」一帧
 *  都不发（2026-09-18 实机曾表现：反馈帧 L=on 而两带强度印成 0/0 自相矛盾）。 */
static void hf_band_strength_survives_into_held_frame(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    event.rumble_on[PAD_TRIGGER_L2] = true;
    event.rumble_on[PAD_TRIGGER_R2] = true;
    event.rumble_hf_strength[PAD_TRIGGER_L2] = 96;
    event.rumble_hf_strength[PAD_TRIGGER_R2] = 96;

    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &held, out, sizeof(out)), 48);
    CHECK_EQ(out[4], 0);  /* b4 左大马达：低频带为 0，不补震 */
    CHECK_EQ(out[3], 96); /* b3 右小马达：跟高频带 */
}

/** 两带驱动频率的落地值随震动事件进持续帧（apply 的拷贝清单陷阱同高频带
 *  强度：漏拷会静默停在 0，音频触觉两侧——板上合成与桥接 FEEDBACK 帧——
 *  就都回落到缺省频率）。 */
static void rumble_frequencies_survive_into_held_frame(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    event.rumble_on[PAD_TRIGGER_L2] = true;
    event.rumble_on[PAD_TRIGGER_R2] = true;
    event.rumble_lf_freq[PAD_TRIGGER_L2] = 55;
    event.rumble_hf_freq[PAD_TRIGGER_L2] = 190;
    event.rumble_lf_freq[PAD_TRIGGER_R2] = 60;
    event.rumble_hf_freq[PAD_TRIGGER_R2] = 200;

    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK_EQ(held.rumble_lf_freq[PAD_TRIGGER_L2], 55);
    CHECK_EQ(held.rumble_hf_freq[PAD_TRIGGER_L2], 190);
    CHECK_EQ(held.rumble_lf_freq[PAD_TRIGGER_R2], 60);
    CHECK_EQ(held.rumble_hf_freq[PAD_TRIGGER_R2], 200);

    /* 玩家灯事件不带震动字段：频率沿用持续帧，不被顺手清掉。 */
    pad_feedback_t led_event = feedback_default();
    led_event.player_led = 0x01;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_PLAYER_LED, &led_event);
    CHECK_EQ(held.rumble_hf_freq[PAD_TRIGGER_R2], 200);
}

/** 主机只发采样 ID、不带播放形态（2026-09-18 实机抓包：重发同一 ID 约
 *  18Hz），节奏由采样音色表给出——0x02（定位呼叫）是首个登记条目：强震、
 *  停顿、两声蜂鸣、停顿，整周期循环；把恒定强度写马达会整段钉成「一直震」。
 *  音色表是常规数据：新增采样只登记新条目，不改编码路径。 */
static void registered_samples_play_their_own_rhythm(void)
{
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 0), 0xC0);    /* 起手段：强震 */
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 219), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 220), 0x00);  /* 停顿段 */
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 400), 0x80);  /* 蜂鸣段 */
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 500), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 600), 0x80);  /* 第二声蜂鸣 */
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 700), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 1199), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 1200), 0xC0); /* 整周期循环 */
    CHECK_EQ(pad_haptic_pulse_envelope(0x02, 1300), 0xC0); /* 落回强震段 */
}

/** 未登记的采样回落缺省音色：一次短脉冲后静默、不循环——重发同一 ID 不
 *  重启节奏，主机要重复播放就用 0x00 收掉再发，缺省不会把整段重发期
 *  钉在震动上。 */
static void unregistered_samples_fall_back_to_one_pulse(void)
{
    CHECK_EQ(pad_haptic_pulse_envelope(0x1A, 0), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x1A, 119), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x1A, 120), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x1A, 1000), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x1A, 1300), 0x00); /* 不循环 */
}

/** 协议清单里的 0x01（低频蜂鸣，约 1 秒）按文档时长登记为音色数据：
 *  一段强震后静默、不循环；实机回填前先钉住数据契约。 */
static void lf_beep_sample_plays_documented_duration(void)
{
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 0), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 999), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 1000), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 3000), 0x00); /* 不循环 */
}

/** 编码侧的采样字节是数据面渲染出的脉冲幅度：音色走强它就强、走到停顿段
 *  马达清零——不再用恒定 0xC0 把整段播放期钉成连续震动。 */
static void sample_pulse_follows_envelope_amplitude(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.haptic_sample_valid = true;
    feedback.haptic_sample = 0x40;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out)),
             32);
    CHECK_EQ(out[4], 0x40);
    CHECK_EQ(out[5], 0x40);

    feedback.haptic_sample = 0; /* 音色停顿段：马达清零 */
    pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out));
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x00);
}

/** 「查找手柄」页的蜂鸣由 0x0A 采样流承载，同期的 LRA 参数包只是载波
 *  （高频 1-2/255，不判成在震）。载波包以接近输入上报的频率到达，持续帧
 *  合并若被非采样事件顺手清掉采样，脉冲会被切成 15ms 碎片、甚至在数据面
 *  编码前就被覆盖——实机表现：查找手柄页的震动时有时无（2026-09-18）。
 *  采样只在带它的事件里更新，0x00 是「停止播放」。 */
static void haptic_pulse_survives_rumble_carriers(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    uint8_t out[PAD_OUTPUT_MAX];

    event.haptic_sample_valid = true;
    event.haptic_sample = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);

    /* 载波包（未判成在震）与玩家灯事件都不能掐掉脉冲。 */
    event = feedback_default();
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_PLAYER_LED, &event);
    CHECK(held.haptic_sample_valid);
    CHECK_EQ(held.haptic_sample, 0x02);
    render_haptic_envelope(&held, 0);
    pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &held, out, sizeof(out));
    CHECK_EQ(out[4], 0xC0);
    CHECK_EQ(out[5], 0xC0);

    /* 停止采样（0x00）把马达收掉。 */
    event.haptic_sample_valid = true;
    event.haptic_sample = 0x00;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);
    pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &held, out, sizeof(out));
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x00);
}

/** DualSense 的 USB 音频接口后两路直连触觉音圈（PS5 同款用法），布局行的
 *  audio_haptic 是 usb_audio 接管的依据——丢了标记只会安静地回落 HID 震动，
 *  但板上合成整条通路再也不会启用，这里把标记钉住（Edge 的 PID 一起验）。 */
static void dualsense_usb_row_marks_audio_haptic(void)
{
    pad_feedback_t feedback = feedback_default();
    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0DF2, &feedback, out,
                                           sizeof(out));
    REQUIRE(len > 0);
    const pad_layout_t *layout = pad_feedback_last_layout();
    REQUIRE(layout != NULL);
    CHECK(layout->out.audio_haptic);
}

/** 玩家指示灯按设备自己的灯位模式点亮：DualSense 的五颗灯是一组固定模式，
 *  直写主机掩码会点错灯（2P 该是中间加外两颗，不是 bit1）。 */
static void dualsense_player_led_follows_pattern(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.player_led = 0x02;

    uint8_t out[PAD_OUTPUT_MAX];
    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[46], 0x0A);
    CHECK_EQ(out[47], 0x00); /* 灯条不驱动：玩家号只上四颗白灯 */
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0x00);
    CHECK_BYTES(&out[74], s_crc_ds5_2p, sizeof(s_crc_ds5_2p));

    /* 没有分配玩家号时五颗全灭。 */
    feedback.player_led = 0x00;
    pad_feedback_bt_seq_reset();
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out));
    CHECK_EQ(out[46], 0x00);
}

/** 震动写回不能改灯条颜色（2026-09-18 实机：玩家灯 0x01 时每次震动写回都把
 *  灯条钉成玩家蓝并带「淡出」设置，平时淡回默认白、一震就变深蓝）。DualSense
 *  的玩家号只落四颗白灯，灯条留给 PC 侧管理：valid_flag1 不置灯条位，灯条
 *  设置与 RGB 字节全零。 */
static void dualsense_rumble_leaves_lightbar_alone(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 255;
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)),
             48);
    CHECK_EQ(out[2], 0x10);  /* valid_flag1：只置玩家指示灯 */
    CHECK_EQ(out[39], 0x00); /* 不写灯条设置控制 */
    CHECK_EQ(out[42], 0x00); /* 不写灯条设置值 */
    CHECK_EQ(out[44], 0x04); /* 1P 灯位仍点亮 */
    CHECK_EQ(out[45], 0x00); /* 灯条 RGB 未声明有效 */
    CHECK_EQ(out[46], 0x00);
    CHECK_EQ(out[47], 0x00);

    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[4], 0x10);
    CHECK_EQ(out[41], 0x00);
    CHECK_EQ(out[44], 0x00);
    CHECK_EQ(out[46], 0x04);
    CHECK_EQ(out[47], 0x00);
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0x00);
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

    /* 采样事件出脉冲；载波包不清它——查找手柄页的蜂鸣要靠脉冲撑住。 */
    event = feedback_default();
    event.haptic_sample_valid = true;
    event.haptic_sample = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);
    CHECK(held.haptic_sample_valid);
    render_haptic_envelope(&held, 0);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0xC0);

    event = feedback_default();
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK(held.haptic_sample_valid); /* 载波不掐脉冲 */
    render_haptic_envelope(&held, 0);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0xC0);

    /* 停止采样（0x00）收掉脉冲，玩家灯仍然保留。 */
    event = feedback_default();
    event.haptic_sample_valid = true;
    event.haptic_sample = 0x00;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);
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

/** DualSense 蓝牙输出报告 b1 的高半字节是序号，每份报告都要递增、低半字节
 *  是 tag 保持 0（Linux hid-playstation.c 的 DS_OUTPUT_SEQ_NO：「needs to be
 *  increased every report」）。恒 0 的报告会被手柄按重复包处理——2026-09-19
 *  蓝牙震动不稳定的头号嫌疑。 */
static void bt_reports_increment_seq_nibble(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 64;
    uint8_t out[PAD_OUTPUT_MAX];

    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)), 78);
    CHECK_EQ(out[1], 0x00);
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)), 78);
    CHECK_EQ(out[1], 0x10);
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)), 78);
    CHECK_EQ(out[1], 0x20);

    /* 16 份后回绕（序号只有 4 位）。 */
    pad_feedback_bt_seq_reset();
    for (int i = 0; i < 16; i++) {
        pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out));
    }
    CHECK_EQ(out[1], 0xF0);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out));
    CHECK_EQ(out[1], 0x00);
}

/** DualShock 4 的蓝牙形态没有序号字节（b1 是 hw_control、b2 是音频控制，
 *  内核 dualshock4_output_report_bt 原样）：序号递增只落在 DualSense 上。 */
static void ds4_bt_keeps_static_header(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 64;
    uint8_t out[PAD_OUTPUT_MAX];

    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x09CC, &feedback, out, sizeof(out)), 78);
    CHECK_EQ(out[1], 0xC0);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x09CC, &feedback, out, sizeof(out));
    CHECK_EQ(out[1], 0xC0);
    CHECK_EQ(out[2], 0x00);
}

/** 主机下发的振幅是 NS2 LRA 的线性档位（共振上小档位也摸得到），ERM 马达
 *  （DS5/DS4/Xbox）低占空比整段落在死区——线性直迁让游戏里中低强度的震动
 *  几乎无感（2026-09-19 实机：USB 直插游戏震动非常轻）。感知重映射把非零档
 *  抬出死区（下限约 40）、压平顶端、保持单调。 */
static void host_rumble_amp_is_remapped_perceptually(void)
{
    CHECK_EQ(pad_rumble_perceived(0), 0);
    CHECK_EQ(pad_rumble_perceived(3), 63);
    CHECK_EQ(pad_rumble_perceived(9), 80);
    CHECK_EQ(pad_rumble_perceived(32), 116);
    CHECK_EQ(pad_rumble_perceived(255), 255);

    uint8_t prev = pad_rumble_perceived(1);
    for (uint32_t amp = 2; amp <= 255; amp++) {
        const uint8_t now = pad_rumble_perceived((uint8_t)amp);
        CHECK(now >= prev);
        CHECK(now >= 40);
        prev = now;
    }
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
    render_haptic_envelope(&feedback, 0); /* 数据面渲染步骤：0x05 → 包络幅度 */

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

/** 采样 ID 0x00 是「静音 / 停止播放」（controller.md「控制指令系统」），不是一次播放：
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

/** 游戏里主机以接近输入上报的频率刷震动流，内容常常只差原始参数包的低有效位
 *  （音频式包络逐包都在抖），写回的字节却一模一样：桥接反馈按「写回语义变了
 *  才发」判定——2026-09-18 实机稳态 `强度 9/9` 每秒重发上百条帧、写回风暴把
 *  PC 会话循环拖到转发卡顿，就是拿原始字节当变化判据的结果。等价判定跟两带
 *  强度、使能、玩家灯与「非零」触觉采样走，原始 LRA 参数包不参与。 */
static void equal_frames_follow_writeback_semantics(void)
{
    pad_feedback_t a;
    pad_feedback_t b;
    pad_feedback_defaults(&a);
    pad_feedback_defaults(&b);
    CHECK(pad_feedback_equal(&a, &b));

    /* 只差原始参数包字节：写回内容不变，视为等价。 */
    a.rumble_raw[PAD_TRIGGER_L2][0] = 0x40;
    CHECK(pad_feedback_equal(&a, &b));

    /* 触觉采样 0x00 是「停止播放」：带不带它的标志位不改变马达字节。 */
    a.haptic_sample_valid = true;
    CHECK(pad_feedback_equal(&a, &b));

    /* 真正的语义变化仍要投递：使能、两带强度、玩家灯与非零采样。 */
    a.rumble_on[PAD_TRIGGER_L2] = true;
    CHECK(!pad_feedback_equal(&a, &b));
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    a.rumble_strength[PAD_TRIGGER_L2] = 64; /* 低频带变了 */
    CHECK(!pad_feedback_equal(&a, &b));
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    a.rumble_hf_strength[PAD_TRIGGER_R2] = 32; /* 高频带变了 */
    CHECK(!pad_feedback_equal(&a, &b));
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    a.player_led = 0x01;
    CHECK(!pad_feedback_equal(&a, &b));
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    a.haptic_sample = 0x03; /* 非零采样：一次真实的播放事件 */
    CHECK(!pad_feedback_equal(&a, &b));
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    CHECK(!pad_feedback_equal(NULL, &b));
    CHECK(!pad_feedback_equal(&a, NULL));
    CHECK(pad_feedback_equal(NULL, NULL));
}

HOST_TEST_SUITE(suite_pad_feedback, "pad_feedback",
                {"DualSense 有线的震动、玩家灯与灯条编码", dualsense_usb_encodes_rumble_and_led},
                {"DS4 有线的震动与灯条颜色编码", ds4_usb_encodes_rumble_and_lightbar},
                {"DualSense 蓝牙的震动写回带上帧头与 CRC32", dualsense_bt_encodes_framed_report},
                {"玩家指示灯按设备灯位模式点亮", dualsense_player_led_follows_pattern},
                {"DS4 蓝牙的震动写回带上帧头与 CRC32", ds4_bt_encodes_framed_report},
                {"NS2 手柄原样接收主机的 LRA 参数包", ns2_pad_relays_lra_payload_verbatim},
                {"DualSense 大马达跟低频带、小马达跟高频带", dualsense_motors_follow_rumble_bands},
                {"DualSense 有线行声明音频触觉能力（usb_audio 接管的依据）",
                 dualsense_usb_row_marks_audio_haptic},
                {"高频带强度随事件进持续帧，漏拷会把纹理震成全零",
                 hf_band_strength_survives_into_held_frame},
                {"两带驱动频率随震动事件进持续帧", rumble_frequencies_survive_into_held_frame},
                {"震动写回不改写 DualSense 灯条，玩家号只上白灯",
                 dualsense_rumble_leaves_lightbar_alone},
                {"触觉采样在无采样能力的设备上退化成短震动",
                 haptic_sample_degrades_to_short_pulse},
                {"停止播放的采样不会把马达留在脉冲上", haptic_stop_sample_silences_motors},
                {"持续帧保留玩家灯、采样脉冲不被载波掐断", held_feedback_keeps_steady_state},
                {"查找手柄页的采样脉冲要撑住整个播放期", haptic_pulse_survives_rumble_carriers},
                {"登记的采样音色按各自节奏渲染而不是恒定强度",
                 registered_samples_play_their_own_rhythm},
                {"未登记的采样回落缺省音色：一次短脉冲后静默",
                 unregistered_samples_fall_back_to_one_pulse},
                {"协议清单的 0x01 低频蜂鸣按文档时长登记", lf_beep_sample_plays_documented_duration},
                {"DualSense 蓝牙输出报告的序号逐报递增", bt_reports_increment_seq_nibble},
                {"DualShock 4 蓝牙报告头保持静态（没有序号字节）", ds4_bt_keeps_static_header},
                {"主机震动振幅按感知曲线重映射", host_rumble_amp_is_remapped_perceptually},
                {"编码的采样字节跟随音色幅度（停顿段清零）",
                 sample_pulse_follows_envelope_amplitude},
                {"NS1 的震动按固定头加振幅写入", ns1_rumble_uses_band_template},
                {"未识别设备没有反馈通道", unknown_device_has_no_feedback_channel},
                {"等价反馈帧按写回语义判定（原始参数包不算变化）",
                 equal_frames_follow_writeback_semantics});

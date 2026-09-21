/**
 * 反馈编码（pad/feedback.c）主机端用例：按布局行钉住各家族输出报告的字节布局，
 * 同代透传单独成例（NS2 手柄直接吃主机的 LRA 参数包）；偏移取自公开实现，核对前以用例为准。
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
 *  依次对应「左 255 / 右 128 / 1P」「停止震动 / 1P」「无震动 2P」「DS4 左 64 / 2P」。
 *  DS5 的三组是灯条退出反馈通道后的取值（valid_flag1 只置玩家灯、灯条设置与
 *  RGB 字节全零），并带上喇叭路由的两个使能位与 b10/b40（见
 *  dualsense_rows_route_the_pad_speaker）。 */
static const uint8_t s_crc_ds5_rumble[4] = {0x0b, 0xf2, 0x56, 0xbd};
static const uint8_t s_crc_ds5_stop[4] = {0xf3, 0x17, 0x33, 0x61};
static const uint8_t s_crc_ds5_2p[4] = {0x55, 0xf1, 0x20, 0x69};
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
    CHECK_EQ(out[1], 0xA3); /* valid_flag0：兼容震动 + 关音频触觉 + 更新喇叭音量 + 音频控制 */
    CHECK_EQ(out[2], 0x90); /* valid_flag1：玩家指示灯 + 前级增益更新，灯条不声明有效 */
    CHECK_EQ(out[3], 128);  /* 右小马达 */
    CHECK_EQ(out[4], 255);  /* 左大马达 */
    CHECK_EQ(out[6], 100);  /* 喇叭音量钉在 PS5 缺省档（采样提示音不轻到听不见） */
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
 *  一遍、结果小端写末 4 字节）——缺了这段 CRC，主机整份报告都不认，
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
    CHECK_EQ(out[3], 0xA3); /* 震动 + 关音频触觉 + 更新喇叭音量 + 音频控制 */
    CHECK_EQ(out[4], 0x90); /* 玩家指示灯 + 前级增益更新 */
    CHECK_EQ(out[5], 128); /* 右小马达 */
    CHECK_EQ(out[6], 255); /* 左大马达 */
    CHECK_EQ(out[8], 100); /* 喇叭音量钉在 PS5 缺省档 */
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
 *  都不发（反馈帧 L=on 而两带强度印成 0/0 自相矛盾）。 */
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

/** 主机只发采样 ID、不带播放形态（重发同一 ID 约
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
 *  一段强震后静默、不循环；回填前先钉住数据契约。 */
static void lf_beep_sample_plays_documented_duration(void)
{
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 0), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 999), 0xC0);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 1000), 0x00);
    CHECK_EQ(pad_haptic_pulse_envelope(0x01, 3000), 0x00); /* 不循环 */
}

/** 蜂鸣器按音色段发声：pulse_step 给出当前段的幅度与距下一段边界的毫秒数，
 *  鸣叫时长跟着段走——「强震、停顿、两声蜂鸣、长停顿」的节奏原样落到
 *  板载蜂鸣器上；循环音色按整周期回绕，非循环播完静默。 */
static void pulse_step_reports_amplitude_and_remaining(void)
{
    uint32_t remain = 0;
    CHECK_EQ(pad_haptic_pulse_step(0x02, 0, &remain, NULL), 0xC0);
    CHECK_EQ(remain, 220);
    CHECK_EQ(pad_haptic_pulse_step(0x02, 50, &remain, NULL), 0xC0);
    CHECK_EQ(remain, 170);
    CHECK_EQ(pad_haptic_pulse_step(0x02, 220, &remain, NULL), 0x00); /* 停顿段 */
    CHECK_EQ(pad_haptic_pulse_step(0x02, 450, &remain, NULL), 0x80);
    CHECK_EQ(remain, 50);
    CHECK_EQ(pad_haptic_pulse_step(0x02, 520, &remain, NULL), 0x00); /* 两声蜂鸣之间 */
    CHECK_EQ(remain, 80);
    CHECK_EQ(pad_haptic_pulse_step(0x02, 620, &remain, NULL), 0x80);
    CHECK_EQ(remain, 80);
    CHECK_EQ(pad_haptic_pulse_step(0x02, 1250, &remain, NULL), 0xC0); /* 整周期回绕 */
    CHECK_EQ(remain, 170);

    CHECK_EQ(pad_haptic_pulse_step(0x1A, 0, &remain, NULL), 0xC0); /* 缺省音色 */
    CHECK_EQ(remain, 120);
    CHECK_EQ(pad_haptic_pulse_step(0x1A, 500, &remain, NULL), 0x00); /* 播完静默 */
    CHECK_EQ(remain, 0);
}

/** 定位呼叫的形态（用户可见行为）：先一下强震，随后两声上行短鸣——
 *  Joy-Con 真手柄的提示音是上行双音，本设备的音色表按段给出音高，
 *  强震段不发声（震动归震动、声音归声音）。 */
static void locate_call_chirps_two_rising_tones(void)
{
    uint32_t remain = 0;
    uint16_t tone = 0xFFFF;
    CHECK_EQ(pad_haptic_pulse_step(0x02, 0, &remain, &tone), 0xC0);
    CHECK_EQ(tone, 0); /* 强震段：只震不响 */
    CHECK_EQ(pad_haptic_pulse_step(0x02, 219, &remain, &tone), 0xC0);
    CHECK_EQ(tone, 0);

    CHECK_EQ(pad_haptic_pulse_step(0x02, 400, &remain, &tone), 0x80);
    CHECK_EQ(tone, 880); /* 第一声 */
    CHECK_EQ(pad_haptic_pulse_step(0x02, 499, &remain, &tone), 0x80);
    CHECK_EQ(tone, 880);

    CHECK_EQ(pad_haptic_pulse_step(0x02, 600, &remain, &tone), 0x80);
    CHECK_EQ(tone, 1175); /* 第二声：比第一声高 */
    CHECK_EQ(pad_haptic_pulse_step(0x02, 699, &remain, &tone), 0x80);
    CHECK_EQ(tone, 1175);

    CHECK_EQ(pad_haptic_pulse_step(0x02, 700, &remain, &tone), 0x00);
    CHECK_EQ(tone, 0); /* 长停顿 */

    /* 未登记采样与低频蜂鸣不带音高：回落布局行的 beep_hz 缺省。 */
    CHECK_EQ(pad_haptic_pulse_step(0x1A, 0, &remain, &tone), 0xC0);
    CHECK_EQ(tone, 0);
    CHECK_EQ(pad_haptic_pulse_step(0x01, 0, &remain, &tone), 0xC0);
    CHECK_EQ(tone, 0);
}

/** 查找手柄的强震段在没有音频触觉承载时要能摸得到：折进两侧马达写回
 *  （蓝牙没开 0x32 流、或音频端点打不开时，采样提示只剩马达这一条出路）。
 *  已有的震动不被降档，幅度 0（非强震段）不动马达。 */
static void pulse_segment_folds_into_motors_without_audio_haptics(void)
{
    pad_feedback_t fb;
    pad_feedback_defaults(&fb);

    pad_feedback_fold_pulse_motors(&fb, PAD_HAPTIC_PULSE);
    CHECK_EQ(fb.rumble_on[PAD_TRIGGER_L2], 1);
    CHECK_EQ(fb.rumble_on[PAD_TRIGGER_R2], 1);
    CHECK_EQ(fb.rumble_strength[PAD_TRIGGER_L2], PAD_HAPTIC_PULSE);
    CHECK_EQ(fb.rumble_strength[PAD_TRIGGER_R2], PAD_HAPTIC_PULSE);

    /* 正在震的一侧取 max：折进不降档。 */
    fb.rumble_strength[PAD_TRIGGER_R2] = 255;
    pad_feedback_fold_pulse_motors(&fb, PAD_HAPTIC_PULSE);
    CHECK_EQ(fb.rumble_strength[PAD_TRIGGER_R2], 255);

    /* 非强震段（发声/停顿）：不动马达。 */
    pad_feedback_t quiet;
    pad_feedback_defaults(&quiet);
    pad_feedback_fold_pulse_motors(&quiet, 0);
    CHECK_EQ(quiet.rumble_on[PAD_TRIGGER_L2], 0);
    CHECK_EQ(quiet.rumble_strength[PAD_TRIGGER_L2], 0);
}

/** 采样提示音（0x0A 采样流）是主机点播的声音，真手柄用 HD 马达把它放成声，
 *  本设备不再把它转成马达震动：编码层对采样字节视而不见，马达只跟 0x30
 *  震动载波走——USB 直插时提示音由板载蜂鸣器发声，蓝牙桥接直接丢弃。 */
static void haptic_sample_never_drives_motors(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.haptic_sample_valid = true;
    feedback.haptic_sample = 0x40;

    uint8_t out[PAD_OUTPUT_MAX];
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out)),
             32);
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x00);

    /* 主机已经在震时马达照常跟震动流，采样不叠加。 */
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 32;
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &feedback, out, sizeof(out)),
             32);
    CHECK_EQ(out[5], 32);
    CHECK_EQ(out[4], 0x00);
}

/** 「查找手柄」页的蜂鸣由 0x0A 采样流承载，同期的 LRA 参数包只是载波
 *  （高频 1-2/255，不判成在震）。载波包以接近输入上报的频率到达，持续帧
 *  合并若被非采样事件顺手清掉采样，蜂鸣节奏会被切成 15ms 碎片——表现：
 *  查找手柄页的提示音时有时无。采样只在带它的事件里更新，
 *  0x00 是「停止播放」；马达不吃采样，一直保持中性。 */
static void haptic_pulse_survives_rumble_carriers(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    uint8_t out[PAD_OUTPUT_MAX];

    event.haptic_sample_valid = true;
    event.haptic_sample = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);

    /* 载波包（未判成在震）与玩家灯事件都不能掐掉采样。 */
    event = feedback_default();
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_PLAYER_LED, &event);
    CHECK(held.haptic_sample_valid);
    CHECK_EQ(held.haptic_sample, 0x02);
    pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x09CC, &held, out, sizeof(out));
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x00);

    /* 停止采样（0x00）照常不惊动马达。 */
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

/** 震动写回不能改灯条颜色（玩家灯 0x01 时每次震动写回都把
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
    CHECK_EQ(out[2], 0x90);  /* valid_flag1：玩家指示灯 + 前级增益更新 */
    CHECK_EQ(out[39], 0x00); /* 不写灯条设置控制 */
    CHECK_EQ(out[42], 0x00); /* 不写灯条设置值 */
    CHECK_EQ(out[44], 0x04); /* 1P 灯位仍点亮 */
    CHECK_EQ(out[45], 0x00); /* 灯条 RGB 未声明有效 */
    CHECK_EQ(out[46], 0x00);
    CHECK_EQ(out[47], 0x00);

    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[4], 0x90);
    CHECK_EQ(out[41], 0x00);
    CHECK_EQ(out[44], 0x00);
    CHECK_EQ(out[46], 0x04);
    CHECK_EQ(out[47], 0x00);
    CHECK_EQ(out[48], 0x00);
    CHECK_EQ(out[49], 0x00);
}

/** 音频触觉让位期间的写回把音圈交还给音频触觉：手柄的音圈模式是粘性的，
 *  HAPTICS_SELECT（0x02）置位后停在震动仿真模式、之后送进去的触觉 PCM 被
 *  静音。让位版本因此不能照抄完整预置的 0xA3，也不能把 valid_flag0 整个留零
 *  （留零等于不交还：马达字节已清零、音圈还停在震动仿真模式，表现是
 *  「没有震动、只剩玩家灯」）——布局行的 quiet_presets 取 0xA1：带
 *  COMPATIBLE_VIBRATION（0x01）而不带 HAPTICS_SELECT。 */
static void quiet_writeback_hands_the_coils_back_to_audio_haptics(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.player_led = 0x01;

    uint8_t out[PAD_OUTPUT_MAX];
    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode_quiet(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback,
                                       out, sizeof(out)),
             78);
    CHECK_EQ(out[0], 0x31);
    CHECK_EQ(out[2], 0x10); /* tag 字节照常 */
    CHECK_EQ(out[3], 0xA1); /* valid_flag0：交还音圈，不留 HAPTICS_SELECT */
    CHECK_EQ(out[4], 0x90); /* valid_flag1：玩家指示灯 + 音频控制 2 照旧 */
    CHECK_EQ(out[5], 0x00); /* 马达字节恒零 */
    CHECK_EQ(out[6], 0x00);
    CHECK_EQ(out[8], 100); /* 喇叭音量档照旧 */
    CHECK_EQ(out[10], 0x30); /* 音频控制（输出路径 = 手柄喇叭）照旧 */
    CHECK_EQ(out[40], 0x02); /* 前级增益照旧 */
    CHECK_EQ(out[46], 0x04); /* 1P 灯位照常点亮 */

    /* 有线行同一份口径：板载合成接手音圈时 0x02 写回也把音圈交还。 */
    CHECK_EQ(pad_feedback_encode_quiet(PAD_CONN_USB, 0x054C, 0x0DF2, &feedback,
                                       out, sizeof(out)),
             48);
    CHECK_EQ(out[0], 0x02);
    CHECK_EQ(out[1], 0xA1);
    CHECK_EQ(out[2], 0x90);
    CHECK_EQ(out[3], 0x00);
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[6], 100);
    CHECK_EQ(out[8], 0x30);
    CHECK_EQ(out[38], 0x02);
    CHECK_EQ(out[44], 0x04);

    /* 完整形态仍带路由与音量档（会话开始那一份，喇叭靠它出声）。 */
    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out, sizeof(out)),
             78);
    CHECK_EQ(out[3], 0xA3);
    CHECK_EQ(out[8], 100);
    CHECK_EQ(out[10], 0x30);
    CHECK_EQ(out[40], 0x02);
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

    /* 采样事件不再驱动马达；载波包不清它——蜂鸣节奏要靠持续帧撑住。 */
    event = feedback_default();
    event.haptic_sample_valid = true;
    event.haptic_sample = 0x02;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_HAPTIC, &event);
    CHECK(held.haptic_sample_valid);
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0x00);

    event = feedback_default();
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK(held.haptic_sample_valid); /* 载波不掐采样 */
    pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &held, out, sizeof(out));
    CHECK_EQ(out[5], 0x00);

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
 *  increased every report」）。恒 0 的报告会被手柄按重复包处理——
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
 *  几乎无感（USB 直插游戏震动非常轻）。感知重映射把非零档
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

/** NS2 的震动是波形描述（每侧 3 个时序子帧，按时间顺序各播 1/3 周期），HD
 *  映射按布局行的 hd 规则把它逐帧重整成设备子帧：振幅线性直迁（10 位压 8
 *  位）、频率按布局范围夹取与回落，时间轴原样保留。这里用 DualSense 的
 *  USB 行钉住子帧表。 */
static void hd_render_maps_host_waveform_per_layout(void)
{
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *ds5 = pad_layout_find_by_ids(0x054C, 0x0CE6, PAD_CONN_USB, &family);
    REQUIRE(ds5 != NULL);
    REQUIRE(ds5->out.hd.ops == 3);

    pad_feedback_t feedback = feedback_default();
    feedback.rumble_key_count[PAD_TRIGGER_L2] = 3;
    feedback.rumble_key_count[PAD_TRIGGER_R2] = 3;
    pad_rumble_key_t *left = feedback.rumble_keys[PAD_TRIGGER_L2];
    left[0].lf_freq = 55;
    left[0].lf_amp = 400; /* 压 8 位 = 100 */
    left[0].hf_freq = 190;
    left[0].hf_amp = 256; /* 压 8 位 = 64 */
    left[1].lf_freq = 90;
    left[1].lf_amp = 8; /* 压 8 位 = 2 */
    left[2].lf_freq = 600; /* 越上界：夹回 500 */
    left[2].lf_amp = 40;   /* 压 8 位 = 10 */
    pad_rumble_key_t *right = feedback.rumble_keys[PAD_TRIGGER_R2];
    right[0].hf_freq = 0;   /* 频率 0 回落缺省 135 */
    right[0].hf_amp = 512;  /* 压 8 位 = 128 */

    pad_hd_render_t render;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key_count[0], 3);
    CHECK_EQ(render.key_count[1], 3);
    CHECK_EQ(render.key[0][0].lf_freq, 55);
    CHECK_EQ(render.key[0][0].lf_gain, 255); /* 100 × 4 过满幅：夹回 */
    CHECK_EQ(render.key[0][0].hf_freq, 190);
    CHECK_EQ(render.key[0][0].hf_gain, 255); /* 64 × 4 过满幅：夹回 */
    CHECK_EQ(render.key[0][1].lf_freq, 90);
    CHECK_EQ(render.key[0][1].lf_gain, 8); /* 2 × 4：布局行 hd 的增益 */
    CHECK_EQ(render.key[0][2].lf_freq, 500); /* 越界夹取 */
    CHECK_EQ(render.key[0][2].lf_gain, 40); /* 10 × 4 */
    CHECK_EQ(render.key[1][0].hf_freq, 135); /* 频率 0 回落缺省 */
    CHECK_EQ(render.key[1][0].hf_gain, 255); /* 128 × 4 过满幅：夹回 */
    CHECK_EQ(render.speaker.freq, 0); /* 没有真正的声音时扬声器静音 */
    CHECK_EQ(render.speaker.gain, 0);

    /* 只声明 1 个有效子帧（载波包的操作数计数）：其余子帧按静默播。 */
    feedback.rumble_key_count[PAD_TRIGGER_L2] = 1;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key_count[0], 1);
    CHECK_EQ(render.key[0][1].lf_gain, 0);
    CHECK_EQ(render.key[0][1].lf_freq, 0);
}

/** 采样音色的段铺色按「震动映射为震动、音频映射为音频」走：强震段以
 *  pulse_hz 覆盖各子帧的音圈（保持子帧时间轴），发声段以 beep_hz 铺到
 *  扬声器，停顿段两者皆静。 */
static void hd_render_spreads_sample_segments(void)
{
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *ds5 = pad_layout_find_by_ids(0x054C, 0x0CE6, PAD_CONN_USB, &family);
    REQUIRE(ds5 != NULL);

    pad_feedback_t feedback = feedback_default();
    feedback.haptic_env = PAD_HAPTIC_PULSE;
    pad_hd_render_t render;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key_count[0], 3);
    for (size_t k = 0; k < 3; k++) {
        CHECK_EQ(render.key[0][k].lf_freq, 135);
        CHECK_EQ(render.key[0][k].lf_gain, 255);
        CHECK_EQ(render.key[0][k].hf_gain, 0);
    }
    CHECK_EQ(render.key_count[1], 3);
    CHECK_EQ(render.speaker.gain, 0);

    /* 主机载波包声明过 1 个子帧（实抓 94% 的包如此）时，合成的强震段仍要
     *  声明满 3 个：声明数决定消费侧的轮播长度，留着 1 会把这一段切成
     *  「5ms 有声 + 10ms 静默」的断续（查找手柄页听着像普通马达）。 */
    feedback.rumble_key_count[PAD_TRIGGER_L2] = 1;
    feedback.rumble_key_count[PAD_TRIGGER_R2] = 1;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key_count[0], 3);
    CHECK_EQ(render.key_count[1], 3);
    CHECK_EQ(render.key[0][2].lf_freq, 135);
    CHECK_EQ(render.key[0][2].lf_gain, 255);
    feedback.rumble_key_count[PAD_TRIGGER_L2] = 0;
    feedback.rumble_key_count[PAD_TRIGGER_R2] = 0;

    feedback.haptic_env = PAD_HAPTIC_BEEP;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key[0][0].lf_gain, 0);
    CHECK_EQ(render.speaker.freq, 500);
    CHECK_EQ(render.speaker.gain, 255);

    /* 音色表带音高的段落（定位呼叫的两声上行短鸣）优先于布局缺省。 */
    feedback.haptic_tone_hz = 880;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.speaker.freq, 880);
    CHECK_EQ(render.speaker.gain, 255);
    feedback.haptic_tone_hz = 1175;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.speaker.freq, 1175);
    feedback.haptic_tone_hz = 0;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.speaker.freq, 500);

    feedback.haptic_env = 0;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key[0][0].lf_gain, 0);
    CHECK_EQ(render.speaker.freq, 0);
}

/** 没有 HD 通路声明的布局行（DualShock 4）不产子帧：HD 映射只发生在声明了
 *  规则的设备上，其余设备继续走马达字节。 */
static void hd_render_needs_a_declared_layout(void)
{
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *ds4 = pad_layout_find_by_ids(0x054C, 0x09CC, PAD_CONN_USB, &family);
    REQUIRE(ds4 != NULL);
    CHECK_EQ(ds4->out.hd.ops, 0);

    pad_feedback_t feedback = feedback_default();
    feedback.rumble_keys[0][0].lf_amp = 400;
    feedback.haptic_env = PAD_HAPTIC_BEEP;
    pad_hd_render_t render;
    pad_feedback_hd_render(ds4, &feedback, &render);
    CHECK_EQ(render.key_count[0], 0);
    CHECK_EQ(render.speaker.freq, 0);

    pad_feedback_hd_render(NULL, &feedback, &render);
    CHECK_EQ(render.key_count[0], 0);
}

/** 反馈状态线格式：基础段 16 字节（老 PC 按长度识别），布局行声明 HD 后
 *  扩到 57 字节——每侧时序子帧表与扬声器音色按固定偏移落位。 */
static void feedback_wire_carries_legacy_and_hd(void)
{
    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_strength[PAD_TRIGGER_L2] = 200;
    feedback.rumble_hf_strength[PAD_TRIGGER_R2] = 40;
    feedback.player_led = 0x01;

    uint8_t wire[64];
    memset(wire, 0xCC, sizeof(wire));
    CHECK_EQ(pad_feedback_wire(&feedback, NULL, wire, sizeof(wire)), PAD_FEEDBACK_WIRE_LEGACY);
    CHECK_EQ(wire[0], 1);
    CHECK_EQ(wire[2], 200);
    CHECK_EQ(wire[7], 40);
    CHECK_EQ(wire[4], 0x01);
    CHECK_EQ(wire[16], 0xCC); /* 无 HD 段：基础段之外一字节都不写 */

    pad_hd_render_t render;
    memset(&render, 0, sizeof(render));
    render.key_count[0] = 2;
    render.key[0][0].lf_freq = 55;
    render.key[0][0].lf_gain = 100;
    render.key[0][1].hf_freq = 190;
    render.key[0][1].hf_gain = 64;
    render.key_count[1] = 1;
    render.key[1][0].hf_freq = 484;
    render.key[1][0].hf_gain = 128;
    render.speaker.freq = 880;
    render.speaker.gain = 255;
    CHECK_EQ(pad_feedback_wire(&feedback, &render, wire, sizeof(wire)), PAD_FEEDBACK_WIRE_HD);
    CHECK_EQ(wire[16], 2);           /* 左侧子帧数 */
    CHECK_EQ(wire[17], 55);          /* 子帧 0 低频频率低字节 */
    CHECK_EQ(wire[19], 100);         /* 子帧 0 低频增益 */
    CHECK_EQ(wire[26], 190);         /* 子帧 1 高频频率低字节 */
    CHECK_EQ(wire[28], 64);
    CHECK_EQ(wire[35], 1);           /* 右侧子帧数 */
    CHECK_EQ(wire[39], 484 & 0xFF);  /* 右侧子帧 0 高频频率 */
    CHECK_EQ(wire[41], 128);
    CHECK_EQ(wire[54], 880 & 0xFF);  /* 扬声器频率低字节 */
    CHECK_EQ(wire[55], 880 >> 8);
    CHECK_EQ(wire[56], 255);

    /* 容量不够装 HD 段时拒绝编码，不写半帧。 */
    CHECK_EQ(pad_feedback_wire(&feedback, &render, wire, PAD_FEEDBACK_WIRE_LEGACY), 0);
}

/** 等价判定要跟上 HD 包络又不被低有效位灌爆：子帧的量化值（振幅 16 档、
 *  频率 64 档）与有效子帧数参与比较，段边界（蜂鸣/停顿切换）也触发投递。 */
static void equal_follows_quantized_ops_and_env(void)
{
    pad_feedback_t a;
    pad_feedback_t b;
    pad_feedback_defaults(&a);
    pad_feedback_defaults(&b);
    a.rumble_keys[0][0].lf_amp = 400;
    b = a;
    CHECK(pad_feedback_equal(&a, &b));

    b.rumble_keys[0][0].lf_amp = 407; /* 低有效位抖动：量化后等价 */
    CHECK(pad_feedback_equal(&a, &b));

    b.rumble_keys[0][0].lf_amp = 500; /* 跨过量化档：真实的包络变化 */
    CHECK(!pad_feedback_equal(&a, &b));

    b = a;
    b.rumble_key_count[0] = 1; /* 有效子帧数变了：时间轴变了 */
    CHECK(!pad_feedback_equal(&a, &b));

    b = a;
    b.haptic_env = PAD_HAPTIC_BEEP; /* 段边界要投递给 HD 通路 */
    CHECK(!pad_feedback_equal(&a, &b));

    b = a;
    b.haptic_env = PAD_HAPTIC_BEEP;
    b.haptic_tone_hz = 880;
    a.haptic_env = PAD_HAPTIC_BEEP;
    a.haptic_tone_hz = 880;
    CHECK(pad_feedback_equal(&a, &b));
    /* 定位呼叫的两声蜂鸣同为发声段：第二声换音高也必须投递，否则 FEEDBACK
     * 会把第一声的音高一直铺下去。 */
    b.haptic_tone_hz = 1175;
    CHECK(!pad_feedback_equal(&a, &b));
}

/** 时序子帧随震动事件进持续帧：apply 漏拷会让 HD 映射停在旧包络上（与高频
 *  带强度同一类陷阱）。 */
static void rumble_keys_survive_into_held_frame(void)
{
    pad_feedback_t held = feedback_default();
    pad_feedback_t event = feedback_default();
    event.rumble_key_count[PAD_TRIGGER_R2] = 3;
    event.rumble_keys[PAD_TRIGGER_R2][2].hf_freq = 300;
    event.rumble_keys[PAD_TRIGGER_R2][2].hf_amp = 360;

    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_RUMBLE, &event);
    CHECK_EQ(held.rumble_key_count[PAD_TRIGGER_R2], 3);
    CHECK_EQ(held.rumble_keys[PAD_TRIGGER_R2][2].hf_freq, 300);
    CHECK_EQ(held.rumble_keys[PAD_TRIGGER_R2][2].hf_amp, 360);

    /* 玩家灯事件不带震动字段：子帧沿用持续帧。 */
    pad_feedback_t led_event = feedback_default();
    led_event.player_led = 0x01;
    pad_feedback_apply(&held, PAD_FEEDBACK_FIELD_PLAYER_LED, &led_event);
    CHECK_EQ(held.rumble_key_count[PAD_TRIGGER_R2], 3);
    CHECK_EQ(held.rumble_keys[PAD_TRIGGER_R2][2].hf_amp, 360);
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

/** 采样 ID 0x00 是「静音 / 停止播放」，不是一次播放：
 *  主机用它收掉「寻找手柄」的提示音时，蜂鸣器与马达都必须停。 */
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
 *  才发」判定——稳态 `强度 9/9` 每秒重发上百条帧、写回风暴把
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

/** 手柄喇叭的输出路径要显式路由：音频控制字节的输出路径位段（bit4-5）不置成
 *  手柄喇叭时，手柄的内置喇叭是「未路由」状态，往它送的声音全被丢掉——
 *  表现是蓝牙私有流里触觉可达而喇叭无声（USB 直插的 4ch 发声段同一回事）。
 *  更新使能位（valid_flag0 bit7、valid_flag1 bit7）不置的话这两个字节不生效，
 *  前级增益同理（+6dB，手柄喇叭的出厂档）。 */
static void dualsense_rows_route_the_pad_speaker(void)
{
    pad_feedback_t feedback = feedback_default();
    uint8_t out[PAD_OUTPUT_MAX];

    /* 有线 0x02：公共段从 b1 起——b1/b2 是有效位，b6 喇叭音量、b8 音频控制、
     * b38 前级增益。 */
    CHECK_EQ(pad_feedback_encode(PAD_CONN_USB, 0x054C, 0x0CE6, &feedback, out, sizeof(out)),
             48);
    CHECK_EQ(out[1] & 0x80, 0x80); /* 音频控制更新使能 */
    CHECK_EQ(out[2] & 0x80, 0x80); /* 前级增益更新使能 */
    CHECK_EQ(out[6], 100);         /* 喇叭音量档 */
    CHECK_EQ(out[8], 0x30);        /* 输出路径 = 手柄喇叭（bit4-5 = 0b11） */
    CHECK_EQ(out[38], 0x02);       /* 喇叭前级增益 */

    /* 蓝牙 0x31：比有线多两字节前缀，公共段从 b3 起——b8 喇叭音量、b10 音频
     * 控制、b40 前级增益。 */
    pad_feedback_bt_seq_reset();
    CHECK_EQ(pad_feedback_encode(PAD_CONN_BT, 0x054C, 0x0DF2, &feedback, out,
                                 sizeof(out)),
             78);
    CHECK_EQ(out[3] & 0x80, 0x80);
    CHECK_EQ(out[4] & 0x80, 0x80);
    CHECK_EQ(out[8], 100);
    CHECK_EQ(out[10], 0x30);
    CHECK_EQ(out[40], 0x02);
}

/** HD 通路的振幅增益（布局行 hd 的 num/den）：主机游戏内档位很小（实抓非零
 *  档位中位 21/1023 → 8 位刻度 5/255，占音圈满幅约百分之二），线性直迁到音圈
 *  接近摸不到；DS5 两行取 4 倍。增益在写 FEEDBACK 帧之前落地，
 *  板载合成与 PC 哑渲染因此吃同一份数值；采样强震段本来是满幅，过增益后仍
 *  夹在 255。 */
static void hd_gain_lifts_quiet_amplitudes(void)
{
    pad_family_t family;
    const pad_layout_t *ds5 = pad_layout_find_by_ids(0x054C, 0x0CE6, PAD_CONN_USB, &family);
    REQUIRE(ds5 != NULL);
    REQUIRE(ds5->out.hd.ops == 3);
    CHECK_EQ(ds5->out.hd.gain_num, 4);
    CHECK_EQ(ds5->out.hd.gain_den, 1);

    pad_feedback_t feedback = feedback_default();
    feedback.rumble_on[PAD_TRIGGER_L2] = true;
    feedback.rumble_key_count[PAD_TRIGGER_L2] = 1;
    feedback.rumble_keys[PAD_TRIGGER_L2][0].lf_amp = 21;  /* 实抓中位：8 位刻度 5 */
    feedback.rumble_keys[PAD_TRIGGER_L2][0].hf_amp = 20;
    /* 频率留 0：按布局规则回落缺省 80/135，增益不参与频率。 */
    feedback.rumble_keys[PAD_TRIGGER_L2][0].lf_freq = 0;
    feedback.rumble_keys[PAD_TRIGGER_L2][0].hf_freq = 0;
    feedback.rumble_key_count[PAD_TRIGGER_R2] = 1;
    feedback.rumble_keys[PAD_TRIGGER_R2][0].lf_amp = 1023; /* 满档：过增益夹回 255 */

    pad_hd_render_t render;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key[0][0].lf_gain, 20);  /* 5 × 4 */
    CHECK_EQ(render.key[0][0].hf_gain, 20);  /* 5 × 4 */
    CHECK_EQ(render.key[1][0].lf_gain, 255); /* 255 × 4 夹回满幅 */
    CHECK_EQ(render.key[0][0].lf_freq, 80);
    CHECK_EQ(render.key[0][0].hf_freq, 135);
    /* 桥接 FEEDBACK 帧带的就是抬过的值：PC 只做哑渲染，自己再乘会双倍。 */
    uint8_t wire[PAD_FEEDBACK_WIRE_HD];
    CHECK_EQ(pad_feedback_wire(&feedback, &render, wire, sizeof(wire)),
             PAD_FEEDBACK_WIRE_HD);
    CHECK_EQ(wire[16], 1);  /* 左侧有效子帧数 */
    CHECK_EQ(wire[19], 20); /* 子帧 0 低频增益（已过增益） */
    CHECK_EQ(wire[22], 20); /* 子帧 0 高频增益 */

    /* 采样强震段是满幅铺色，增益不改变它。 */
    feedback.haptic_env = PAD_HAPTIC_PULSE;
    pad_feedback_hd_render(ds5, &feedback, &render);
    CHECK_EQ(render.key[0][0].lf_gain, 255);
    CHECK_EQ(render.key[1][2].lf_gain, 255);
}

/** 采样音色的段边界按 tick 投递：段是固件合成的（主机只给采样 ID 与起停），
 *  投递原先只在主机事件到达时发生——查找手柄页的采样事件约 15Hz，段边界被
 *  量化到 64ms 的栅格（震动/蜂鸣起止错位、短段整段丢失）。判据只看已投递的
 *  段状态与当前的差异，段音高只在「发声」段参与。 */
static void sample_segment_delivery_tracks_the_tick(void)
{
    pad_feedback_t sent = feedback_default();
    sent.haptic_env = PAD_HAPTIC_PULSE;
    sent.haptic_tone_hz = 0;
    CHECK(!pad_feedback_segment_changed(&sent, PAD_HAPTIC_PULSE, 0));
    CHECK(pad_feedback_segment_changed(&sent, 0, 0));            /* 强震 → 停顿 */
    sent.haptic_env = 0;
    CHECK(pad_feedback_segment_changed(&sent, PAD_HAPTIC_BEEP, 880));
    sent.haptic_env = PAD_HAPTIC_BEEP;
    sent.haptic_tone_hz = 880;
    CHECK(pad_feedback_segment_changed(&sent, PAD_HAPTIC_BEEP, 1175)); /* 第二声换音高 */
    CHECK(!pad_feedback_segment_changed(&sent, PAD_HAPTIC_BEEP, 880));
    sent.haptic_env = PAD_HAPTIC_PULSE;
    sent.haptic_tone_hz = 0;
    CHECK(!pad_feedback_segment_changed(&sent, PAD_HAPTIC_PULSE, 1175)); /* 非发声段不看音高 */
    CHECK(pad_feedback_segment_changed(NULL, PAD_HAPTIC_PULSE, 0));
}

HOST_TEST_SUITE(suite_pad_feedback, "pad_feedback",
                {"采样音色的段边界按 tick 投递（段与段音高变化才算）",
                 sample_segment_delivery_tracks_the_tick},
                {"HD 通路的振幅增益把安静档位抬进可感知区",
                 hd_gain_lifts_quiet_amplitudes},
                {"手柄喇叭的输出路径显式路由到手柄喇叭",
                 dualsense_rows_route_the_pad_speaker},
                {"音频触觉让位期间的写回把音圈交还给音频触觉",
                 quiet_writeback_hands_the_coils_back_to_audio_haptics},
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
                {"停止播放的采样不会把马达留在脉冲上", haptic_stop_sample_silences_motors},
                {"持续帧保留玩家灯、采样不被载波掐断", held_feedback_keeps_steady_state},
                {"查找手柄页的采样要撑住整个播放期", haptic_pulse_survives_rumble_carriers},
                {"登记的采样音色按各自节奏渲染而不是恒定强度",
                 registered_samples_play_their_own_rhythm},
                {"未登记的采样回落缺省音色：一次短脉冲后静默",
                 unregistered_samples_fall_back_to_one_pulse},
                {"协议清单的 0x01 低频蜂鸣按文档时长登记", lf_beep_sample_plays_documented_duration},
                {"采样音色按段给出幅度与剩余毫秒（蜂鸣器按段发声）",
                 pulse_step_reports_amplitude_and_remaining},
                {"没有音频承载时查找手柄的强震段折进马达",
                 pulse_segment_folds_into_motors_without_audio_haptics},
                {"定位呼叫先强震再两声上行短鸣（近似 Joy-Con 提示音）",
                 locate_call_chirps_two_rising_tones},
                {"DualSense 蓝牙输出报告的序号逐报递增", bt_reports_increment_seq_nibble},
                {"DualShock 4 蓝牙报告头保持静态（没有序号字节）", ds4_bt_keeps_static_header},
                {"主机震动振幅按感知曲线重映射", host_rumble_amp_is_remapped_perceptually},
                {"采样提示音不驱动马达（蜂鸣器与丢弃负责发声）",
                 haptic_sample_never_drives_motors},
                {"NS1 的震动按固定头加振幅写入", ns1_rumble_uses_band_template},
                {"未识别设备没有反馈通道", unknown_device_has_no_feedback_channel},
                {"等价反馈帧按写回语义判定（原始参数包不算变化）",
                 equal_frames_follow_writeback_semantics},
                {"HD 映射按布局规则把主机波形重整成时序子帧",
                 hd_render_maps_host_waveform_per_layout},
                {"采样音色的强震段铺音圈、发声段铺扬声器",
                 hd_render_spreads_sample_segments},
                {"没有声明 HD 通路的布局行不产子帧", hd_render_needs_a_declared_layout},
                {"反馈线格式带基础段与 HD 子帧段", feedback_wire_carries_legacy_and_hd},
                {"等价判定跟子帧量化值与采样段边界走",
                 equal_follows_quantized_ops_and_env},
                {"时序子帧随震动事件进持续帧", rumble_keys_survive_into_held_frame});

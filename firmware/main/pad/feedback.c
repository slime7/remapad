#include "feedback.h"

#include <string.h>

/** NS2 输出报告：USB 形态的 0x02 是报告 ID + 左右 LRA 各 16 字节 + 9 字节保留。 */
#define PAD_NS2_OUT_REPORT_ID 0x02u
#define PAD_NS2_OUT_LRA_LEN 42u

/**
 * 感知重映射表：out = 40 + 215·√(amp/255)（amp > 0，四舍五入），0 除外。
 * NS2 的线性档位直写 ERM 马达时小值整段落在死区，这里抬低端、压顶端；
 * 非零档的落地值不低于 53。
 */
static const uint8_t s_perceived[256] = {
      0,  53,  59,  63,  67,  70,  73,  76,  78,  80,  83,  85,
     87,  89,  90,  92,  94,  96,  97,  99, 100, 102, 103, 105,
    106, 107, 109, 110, 111, 113, 114, 115, 116, 117, 119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 143,
    144, 145, 146, 147, 148, 149, 149, 150, 151, 152, 153, 153,
    154, 155, 156, 157, 157, 158, 159, 160, 160, 161, 162, 163,
    163, 164, 165, 166, 166, 167, 168, 168, 169, 170, 171, 171,
    172, 173, 173, 174, 175, 175, 176, 177, 177, 178, 179, 179,
    180, 181, 181, 182, 182, 183, 184, 184, 185, 186, 186, 187,
    187, 188, 189, 189, 190, 191, 191, 192, 192, 193, 194, 194,
    195, 195, 196, 196, 197, 198, 198, 199, 199, 200, 200, 201,
    202, 202, 203, 203, 204, 204, 205, 205, 206, 207, 207, 208,
    208, 209, 209, 210, 210, 211, 211, 212, 212, 213, 213, 214,
    215, 215, 216, 216, 217, 217, 218, 218, 219, 219, 220, 220,
    221, 221, 222, 222, 223, 223, 224, 224, 225, 225, 226, 226,
    227, 227, 228, 228, 228, 229, 229, 230, 230, 231, 231, 232,
    232, 233, 233, 234, 234, 235, 235, 236, 236, 236, 237, 237,
    238, 238, 239, 239, 240, 240, 241, 241, 242, 242, 242, 243,
    243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248, 248,
    249, 249, 249, 250, 250, 251, 251, 252, 252, 252, 253, 253,
    254, 254, 255, 255,
};

uint8_t pad_rumble_perceived(uint8_t amp)
{
    return s_perceived[amp];
}

/** DualSense 蓝牙输出报告的序号半字节：内核 hid-playstation.c 注明高 4 位
 *  是「每份报告都要递增」的序号（DS_OUTPUT_SEQ_NO），恒值报告会被手柄按
 *  重复包处理——蓝牙震动不稳定的头号嫌疑；低 4 位 tag 保持 0。
 *  USB 形态没有这个字节，因此只有蓝牙路径不稳定。 */
static uint8_t s_ps_bt_seq;

void pad_feedback_bt_seq_reset(void)
{
    s_ps_bt_seq = 0;
}

/** 采样音色的一个幅度段：段内幅度恒定，响/停切换只发生在段边界。 */
typedef struct {
    uint16_t until_ms;
    uint8_t amp;
} haptic_env_step_t;

/** 一档采样音色：周期内按段渲染；loop 决定播完周期后循环还是停住。 */
typedef struct {
    uint8_t sample;
    bool loop;
    const haptic_env_step_t *steps;
    size_t step_count;
} haptic_sound_t;

/** 定位呼叫（0x02，「搜索手柄」长按，实机抓包建模）：真手柄上
 *  「强震、停顿、两声蜂鸣、长停顿」是 HD 马达放出的声与震，这里整段转成
 *  板载蜂鸣器的响/停节奏，整周期 1200ms 循环——主机长按期间持续重发，
 *  节奏只能由设备侧给出，恒定单一响法撑不出这个形态。 */
static const haptic_env_step_t s_snd_locate[] = {
    {220, PAD_HAPTIC_PULSE},
    {400, 0},
    {500, PAD_HAPTIC_BEEP},
    {600, 0},
    {700, PAD_HAPTIC_BEEP},
    {1200, 0},
};

/** 低频蜂鸣（0x01，子命令 0x02 的采样清单：约 1 秒低频蜂鸣）：一段强震
 *  后静默、不循环；时长按协议文档登记，播放形态待实机回填。 */
static const haptic_env_step_t s_snd_lf_beep[] = {
    {1000, PAD_HAPTIC_PULSE},
    {1100, 0},
};

/** 缺省音色：一次短脉冲后静默、不循环——未登记采样的兜底，重发同一 ID
 *  不重启节奏，主机要重复播放就用 0x00 收掉再发。 */
static const haptic_env_step_t s_snd_default[] = {
    {120, PAD_HAPTIC_PULSE},
    {300, 0},
};

/** 采样音色表：按 ID 登记，新增采样只加数据行。 */
static const haptic_sound_t s_haptic_bank[] = {
    {0x02, true, s_snd_locate, sizeof(s_snd_locate) / sizeof(s_snd_locate[0])},
    {0x01, false, s_snd_lf_beep, sizeof(s_snd_lf_beep) / sizeof(s_snd_lf_beep[0])},
};

/** 采样音色在 age_ms 的当前段：返回段内幅度，remain_ms（可空）给出距下一段
 *  边界的毫秒数——蜂鸣器按段发声用，鸣叫时长跟着段走而不是固定值。非循环
 *  音色播完或未登记采样走缺省音色同一条路径，静默段幅度为 0。 */
uint8_t pad_haptic_pulse_step(uint8_t sample, uint32_t age_ms, uint32_t *remain_ms)
{
    const haptic_env_step_t *steps = s_snd_default;
    size_t step_count = sizeof(s_snd_default) / sizeof(s_snd_default[0]);
    bool loop = false;
    for (size_t i = 0; i < sizeof(s_haptic_bank) / sizeof(s_haptic_bank[0]); i++) {
        const haptic_sound_t *sound = &s_haptic_bank[i];
        if (sound->sample == sample) {
            steps = sound->steps;
            step_count = sound->step_count;
            loop = sound->loop;
            break;
        }
    }
    const uint32_t period = steps[step_count - 1].until_ms;
    const uint32_t ms = loop ? age_ms % period : age_ms;
    for (size_t i = 0; i < step_count; i++) {
        if (ms < steps[i].until_ms) {
            if (remain_ms != NULL) {
                *remain_ms = steps[i].until_ms - ms;
            }
            return steps[i].amp;
        }
    }
    if (remain_ms != NULL) {
        *remain_ms = 0;
    }
    return 0;
}

uint8_t pad_haptic_pulse_envelope(uint8_t sample, uint32_t age_ms)
{
    return pad_haptic_pulse_step(sample, age_ms, NULL);
}

/** PS 蓝牙输出报告的 CRC32 种子字节（Linux hid-playstation.c 的
 *  PS_OUTPUT_CRC32_SEED）：它是 hidp 传输头，参与 CRC 计算。 */
#define PAD_PS_BT_CRC_SEED 0xA2u

static const pad_layout_t *s_last_layout;

/** 玩家灯落地值：行里给了映射表就按最低置位取表——DualSense 的五颗灯是一组
 *  固定模式（1P 只有中灯、2P 中灯加外灯），直写主机掩码会点错灯；没给表的
 *  行原样写主机掩码。没有分配玩家号（掩码 0）时写 0，五颗全灭。 */
static uint8_t led_mask_value(const pad_output_layout_t *desc, uint8_t mask)
{
    for (size_t i = 0; i < 4; i++) {
        if ((mask & (uint8_t)(1u << i)) != 0) {
            return desc->led_mask_map[i] != 0 ? desc->led_mask_map[i] : mask;
        }
    }
    return 0;
}

/** CRC32（反射多项式 0xEDB88320、初值 0xFFFFFFFF），与 Linux 的 crc32_le
 *  同形：不做最终取反，由调用方补。 */
static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            const uint32_t mask = (crc & 1u) != 0 ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc;
}

/** PS 蓝牙输出报告的尾帧：先在种子字节上过一遍，再覆盖报告体（末 4 字节除
 *  外），结果小端写进末 4 字节。缺这段的主机不接受整份报告——实机表现是写
 *  回成功、手柄没有任何反应。 */
static void ps_bt_frame(uint8_t *out, size_t len)
{
    const uint8_t seed = (uint8_t)PAD_PS_BT_CRC_SEED;
    uint32_t crc = crc32_le(0xFFFFFFFFu, &seed, 1);
    crc = ~crc32_le(crc, out, len - 4u);
    out[len - 4u] = (uint8_t)(crc & 0xFFu);
    out[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    out[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    out[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
}

/**
 * 偏移是否可用：0 是报告 ID 字节，任何字段都不会落在那里，因此 0 与
 * PAD_OFF_NONE 一样按「没有这个字段」处理，未初始化的行不会写坏报告 ID。
 */
static bool off_set(uint8_t off)
{
    return off != PAD_OFF_NONE && off != 0;
}

/** 玩家灯掩码换算成灯条颜色：取最低置位，四种颜色循环。 */
static void led_color(uint8_t mask, uint8_t *rgb)
{
    static const uint8_t palette[4][3] = {
        {0x00, 0x00, 0xFF}, /* bit0 蓝 */
        {0xFF, 0x00, 0x00}, /* bit1 红 */
        {0x00, 0xFF, 0x00}, /* bit2 绿 */
        {0xFF, 0x00, 0xFF}, /* bit3 品红 */
    };
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;
    for (size_t i = 0; i < 4; i++) {
        if ((mask & (uint8_t)(1u << i)) != 0) {
            rgb[0] = palette[i][0];
            rgb[1] = palette[i][1];
            rgb[2] = palette[i][2];
            return;
        }
    }
}

const pad_layout_t *pad_feedback_last_layout(void)
{
    return s_last_layout;
}

/** 频率落地值（HD 规则）：0 回落该带缺省，再夹进 [min, max]（max 0 = 不设
 *  上限的防呆）。 */
static uint16_t hd_freq(const pad_hd_haptic_t *hd, uint16_t raw, bool high_band)
{
    const uint16_t min = high_band ? hd->hf_min_hz : hd->lf_min_hz;
    const uint16_t max = high_band ? hd->hf_max_hz : hd->lf_max_hz;
    if (raw == 0) {
        raw = high_band ? hd->hf_default_hz : hd->lf_default_hz;
    }
    if (raw < min) {
        return min;
    }
    if (max != 0 && raw > max) {
        return max;
    }
    return raw;
}

void pad_feedback_hd_render(const pad_layout_t *layout, const pad_feedback_t *feedback,
                            pad_hd_render_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (layout == NULL || feedback == NULL) {
        return;
    }
    const pad_hd_haptic_t *hd = &layout->out.hd;
    if (hd->ops == 0 || hd->ops > PAD_HD_KEY_MAX) {
        return;
    }
    for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
        /* 主机波形的时序重整：子帧按时间顺序原样保留（各播 cycle_ms/3），
         * 振幅按原始档位线性直迁（10 位压到 8 位刻度）——音圈没有 ERM 死区，
         * 不做感知重映射，重整只落在频率范围与承载 PCM 上；无效子帧是
         * 静默子帧（增益 0），保住主机排好的时间轴。 */
        size_t count = feedback->rumble_key_count[side];
        if (count == 0 || count > PAD_HD_KEY_MAX) {
            count = PAD_HD_KEY_MAX;
        }
        out->key_count[side] = (uint8_t)count;
        for (size_t k = 0; k < PAD_HD_KEY_MAX; k++) {
            const pad_rumble_key_t *src =
                k < count ? &feedback->rumble_keys[side][k] : &feedback->rumble_keys[side][0];
            const bool active = k < count && (src->lf_amp != 0 || src->hf_amp != 0);
            out->key[side][k].lf_freq = active ? hd_freq(hd, src->lf_freq, false) : 0;
            out->key[side][k].lf_gain = active ? (uint8_t)(src->lf_amp >> 2) : 0;
            out->key[side][k].hf_freq = active ? hd_freq(hd, src->hf_freq, true) : 0;
            out->key[side][k].hf_gain = active ? (uint8_t)(src->hf_amp >> 2) : 0;
        }
        /* 采样「强震」段是音色里的震动成分：覆盖该侧各子帧的音圈（真手柄的
         * 定位音就是 HD 马达放出的「震动、停顿、发声、停顿」，震动段归
         * 音圈），保持子帧时间轴不变。 */
        if (feedback->haptic_env == PAD_HAPTIC_PULSE && hd->pulse_hz != 0) {
            for (size_t k = 0; k < PAD_HD_KEY_MAX; k++) {
                out->key[side][k].lf_freq = hd->pulse_hz;
                out->key[side][k].lf_gain = 255u;
                out->key[side][k].hf_freq = 0;
                out->key[side][k].hf_gain = 0;
            }
        }
    }
    /* 「发声」段铺到扬声器（音频映射为音频）：没有真正声音的时段保持静音。 */
    if (feedback->haptic_env == PAD_HAPTIC_BEEP && hd->beep_hz != 0) {
        out->speaker.freq = hd->beep_hz;
        out->speaker.gain = 255u;
    }
}

size_t pad_feedback_wire(const pad_feedback_t *feedback, const pad_hd_render_t *hd,
                         uint8_t *out, size_t cap)
{
    if (feedback == NULL || out == NULL) {
        return 0;
    }
    if (cap < PAD_FEEDBACK_WIRE_LEGACY) {
        return 0;
    }
    memset(out, 0, PAD_FEEDBACK_WIRE_LEGACY);
    out[0] = feedback->rumble_on[PAD_TRIGGER_L2] ? 1u : 0u;
    out[1] = feedback->rumble_on[PAD_TRIGGER_R2] ? 1u : 0u;
    out[2] = feedback->rumble_strength[PAD_TRIGGER_L2];
    out[3] = feedback->rumble_strength[PAD_TRIGGER_R2];
    out[4] = feedback->player_led;
    out[5] = feedback->haptic_sample_valid ? feedback->haptic_sample : 0u;
    out[6] = feedback->rumble_hf_strength[PAD_TRIGGER_L2];
    out[7] = feedback->rumble_hf_strength[PAD_TRIGGER_R2];
    const uint16_t freqs[4] = {
        feedback->rumble_lf_freq[PAD_TRIGGER_L2],
        feedback->rumble_lf_freq[PAD_TRIGGER_R2],
        feedback->rumble_hf_freq[PAD_TRIGGER_L2],
        feedback->rumble_hf_freq[PAD_TRIGGER_R2],
    };
    for (size_t i = 0; i < 4; i++) {
        out[8 + i * 2] = (uint8_t)(freqs[i] & 0xFFu);
        out[9 + i * 2] = (uint8_t)(freqs[i] >> 8);
    }
    if (hd == NULL) {
        return PAD_FEEDBACK_WIRE_LEGACY;
    }
    if (cap < PAD_FEEDBACK_WIRE_HD) {
        return 0;
    }
    memset(&out[PAD_FEEDBACK_WIRE_LEGACY], 0,
           PAD_FEEDBACK_WIRE_HD - PAD_FEEDBACK_WIRE_LEGACY);
    for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
        const size_t base = side == 0 ? 16u : 35u;
        out[base] = hd->key_count[side];
        for (size_t k = 0; k < PAD_HD_KEY_MAX; k++) {
            uint8_t *p = &out[base + 1 + k * 6];
            p[0] = (uint8_t)(hd->key[side][k].lf_freq & 0xFFu);
            p[1] = (uint8_t)(hd->key[side][k].lf_freq >> 8);
            p[2] = hd->key[side][k].lf_gain;
            p[3] = (uint8_t)(hd->key[side][k].hf_freq & 0xFFu);
            p[4] = (uint8_t)(hd->key[side][k].hf_freq >> 8);
            p[5] = hd->key[side][k].hf_gain;
        }
    }
    out[54] = (uint8_t)(hd->speaker.freq & 0xFFu);
    out[55] = (uint8_t)(hd->speaker.freq >> 8);
    out[56] = hd->speaker.gain;
    return PAD_FEEDBACK_WIRE_HD;
}

void pad_feedback_apply(pad_feedback_t *held, uint8_t fields, const pad_feedback_t *event)
{
    if ((fields & PAD_FEEDBACK_FIELD_RUMBLE) != 0) {
        for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
            held->rumble_on[side] = event->rumble_on[side];
            held->rumble_strength[side] = event->rumble_strength[side];
            held->rumble_hf_strength[side] = event->rumble_hf_strength[side];
            held->rumble_lf_freq[side] = event->rumble_lf_freq[side];
            held->rumble_hf_freq[side] = event->rumble_hf_freq[side];
            memcpy(held->rumble_raw[side], event->rumble_raw[side],
                   sizeof(held->rumble_raw[side]));
            /* 时序子帧随震动事件一起覆盖：HD 映射吃的是它，漏拷会让主机的
             * 波形变化停在旧包络上（与高频带强度同一类陷阱）。 */
            memcpy(held->rumble_keys[side], event->rumble_keys[side],
                   sizeof(held->rumble_keys[side]));
            held->rumble_key_count[side] = event->rumble_key_count[side];
        }
    }
    if ((fields & PAD_FEEDBACK_FIELD_PLAYER_LED) != 0) {
        held->player_led = event->player_led;
    }
    /* 采样只在带它的事件里更新：载波包以接近输入上报的频率到达，非采样事件
     * 顺手清会把脉冲切成碎片、甚至在编码前就把它覆盖掉（查找手柄页的蜂鸣
     * 时有时无）；收尾交给 0x00 的「停止播放」与数据面的超时自灭。 */
    if ((fields & PAD_FEEDBACK_FIELD_HAPTIC) != 0) {
        held->haptic_sample_valid = event->haptic_sample_valid;
        held->haptic_sample = event->haptic_sample_valid ? event->haptic_sample : 0;
    }
}

/** 触觉采样的写回语义：0x00 是「停止播放」，带不带它的标志位不改变任何
 *  马达字节，等价判定要按这个有效值算。 */
static uint8_t effective_haptic(const pad_feedback_t *f)
{
    return f->haptic_sample_valid && f->haptic_sample != 0 ? f->haptic_sample : 0;
}

/** HD 子帧的量化值（等价判定用）：原始子帧逐包在抖（低有效位、频率扫描），
 *  全精度比较会把等价帧全部判成变化；完全忽略又会让 HD 流冻结在旧包络上。
 *  振幅压到 16 档、频率压到 64 档——真实的包络变化仍然触发，低位的抖动
 *  不再产生新帧。 */
static bool key_equal_quantized(const pad_rumble_key_t *a, const pad_rumble_key_t *b)
{
    return (a->lf_amp >> 4) == (b->lf_amp >> 4) && (a->hf_amp >> 4) == (b->hf_amp >> 4) &&
           (a->lf_freq >> 3) == (b->lf_freq >> 3) && (a->hf_freq >> 3) == (b->hf_freq >> 3);
}

bool pad_feedback_equal(const pad_feedback_t *a, const pad_feedback_t *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    /* 只比会改变写回内容的语义字段。主机的震动流是音频式连续包络，原始 LRA
     * 参数包逐包都在抖（低有效位、频率扫描），拿它当变化判据会把等价帧全部
     * 判成变化、以接近输入上报的频率把串口灌爆（实机曾出现稳态强度每秒重发
     * 上百条帧，把 PC 会话循环拖到输入转发卡顿）。 */
    if (memcmp(a->rumble_on, b->rumble_on, sizeof(a->rumble_on)) != 0 ||
        memcmp(a->rumble_strength, b->rumble_strength, sizeof(a->rumble_strength)) != 0 ||
        memcmp(a->rumble_hf_strength, b->rumble_hf_strength,
               sizeof(a->rumble_hf_strength)) != 0 ||
        a->player_led != b->player_led || effective_haptic(a) != effective_haptic(b)) {
        return false;
    }
    /* HD 子帧按量化值参与：HD 流要跟上主机的波形包络，但低位的逐包抖动
     * 不值得占一次传输。 */
    for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
        if (a->rumble_key_count[side] != b->rumble_key_count[side]) {
            return false;
        }
        for (size_t k = 0; k < PAD_RUMBLE_KEY_COUNT; k++) {
            if (!key_equal_quantized(&a->rumble_keys[side][k], &b->rumble_keys[side][k])) {
                return false;
            }
        }
    }
    /* 采样音色的段边界（蜂鸣与停顿切换）也要投递：PC 侧的发声段铺色按它走。 */
    return a->haptic_env == b->haptic_env;
}

size_t pad_feedback_encode(pad_conn_t conn, uint16_t vid, uint16_t pid,
                           const pad_feedback_t *feedback, uint8_t *out, size_t out_len)
{
    s_last_layout = NULL;
    if (feedback == NULL || out == NULL || out_len == 0) {
        return 0;
    }
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *layout = pad_layout_find_by_ids(vid, pid, conn, &family);
    if (layout == NULL) {
        return 0;
    }
    s_last_layout = layout;

    if (layout->native_lang == PAD_LANG_NS2) {
        /* 同代透传：主机下发的 LRA 参数包就是该设备自己的语言。 */
        if (out_len < PAD_NS2_OUT_LRA_LEN) {
            return 0;
        }
        memset(out, 0, PAD_NS2_OUT_LRA_LEN);
        out[0] = PAD_NS2_OUT_REPORT_ID;
        memcpy(&out[1], feedback->rumble_raw[PAD_TRIGGER_L2], 16);
        memcpy(&out[17], feedback->rumble_raw[PAD_TRIGGER_R2], 16);
        return PAD_NS2_OUT_LRA_LEN;
    }

    const pad_output_layout_t *desc = &layout->out;
    if (desc->report_id == 0 || desc->len == 0 || desc->len > out_len) {
        s_last_layout = NULL;
        return 0;
    }
    memset(out, 0, desc->len);
    out[0] = desc->report_id;
    for (size_t i = 0; i < PAD_OUT_PRESET_MAX; i++) {
        const uint8_t off = desc->presets[i][0];
        /* 未填的槽位是 {0, 0}：偏移 0 是报告 ID，一律跳过（同 off_set）。 */
        if (!off_set(off) || off >= desc->len) {
            continue;
        }
        out[off] = desc->presets[i][1];
    }

    /* 马达只跟 0x30 震动载波：触觉采样（0x0A 采样流）是主机点播的声音，
     * 不转成马达震动——USB 直插时由板载蜂鸣器按音色表发声，蓝牙桥接直接
     * 丢弃（编码层对采样字节视而不见）。 */
    for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
        const uint8_t off = desc->rumble_off[side];
        if (!off_set(off) || off >= desc->len) {
            continue;
        }
        const uint16_t max = desc->rumble_max[side] == 0 ? 255u : desc->rumble_max[side];
        /* 这颗马达跟哪条频带：主机震动流的低频给冲击、高频给纹理，两带分开
         * 映射（DS5 大马达跟低频、小马达跟高频），不把同一个值写两颗马达。 */
        const uint8_t source = desc->rumble_band[side] == PAD_RUMBLE_HF
                                   ? feedback->rumble_hf_strength[side]
                                   : feedback->rumble_strength[side];
        out[off] = (uint8_t)((uint16_t)(feedback->rumble_on[side] ? source : 0u) * max / 255u);
    }

    if (desc->led_style != PAD_LED_NONE) {
        if (off_set(desc->led_mask_off) && desc->led_mask_off < desc->len) {
            out[desc->led_mask_off] = led_mask_value(desc, feedback->player_led);
        }
        if (desc->led_style == PAD_LED_LIGHTBAR && off_set(desc->led_rgb_off) &&
            (size_t)desc->led_rgb_off + 2u < desc->len) {
            led_color(feedback->player_led, &out[desc->led_rgb_off]);
        }
    }
    if (desc->frame == PAD_OUT_FRAME_PS_BT) {
        if (desc->len < 5u) {
            /* 连尾部 CRC 都放不下：按没有反馈通道处理，不写坏报告。 */
            s_last_layout = NULL;
            return 0;
        }
        /* 序号半字节逐报递增（低半字节 tag 保持 0），再算覆盖整份报告体的
         * CRC32。 */
        if (off_set(desc->seq_off) && desc->seq_off < desc->len) {
            out[desc->seq_off] = (uint8_t)(s_ps_bt_seq << 4);
            s_ps_bt_seq = (uint8_t)((s_ps_bt_seq + 1u) & 0xFu);
        }
        ps_bt_frame(out, desc->len);
    }
    return desc->len;
}

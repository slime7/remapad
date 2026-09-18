#include "feedback.h"

#include <string.h>

/** NS2 输出报告：USB 形态的 0x02 是报告 ID + 左右 LRA 各 16 字节 + 9 字节保留。 */
#define PAD_NS2_OUT_REPORT_ID 0x02u
#define PAD_NS2_OUT_LRA_LEN 42u

/** 触觉采样退化成的短震动强度（0-255，按行内量程缩放后写入）。 */
#define PAD_HAPTIC_PULSE 0xC0u

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

void pad_feedback_apply(pad_feedback_t *held, uint8_t fields, const pad_feedback_t *event)
{
    if ((fields & PAD_FEEDBACK_FIELD_RUMBLE) != 0) {
        for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
            held->rumble_on[side] = event->rumble_on[side];
            held->rumble_strength[side] = event->rumble_strength[side];
            held->rumble_hf_strength[side] = event->rumble_hf_strength[side];
            memcpy(held->rumble_raw[side], event->rumble_raw[side],
                   sizeof(held->rumble_raw[side]));
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
     * 判成变化、以接近输入上报的频率把串口灌爆（2026-09-18 实机：稳态强度
     * 9/9 每秒重发上百条帧，PC 会话循环被拖到输入转发卡顿）。 */
    return memcmp(a->rumble_on, b->rumble_on, sizeof(a->rumble_on)) == 0 &&
           memcmp(a->rumble_strength, b->rumble_strength, sizeof(a->rumble_strength)) == 0 &&
           memcmp(a->rumble_hf_strength, b->rumble_hf_strength,
                  sizeof(a->rumble_hf_strength)) == 0 &&
           a->player_led == b->player_led && effective_haptic(a) == effective_haptic(b);
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

    const bool rumbling = feedback->rumble_on[PAD_TRIGGER_L2] ||
                          feedback->rumble_on[PAD_TRIGGER_R2];
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
        uint8_t strength = 0;
        if (feedback->rumble_on[side]) {
            strength = source;
        } else if (feedback->haptic_sample_valid && feedback->haptic_sample != 0 &&
                   desc->haptic == PAD_HAPTIC_AS_RUMBLE && !rumbling) {
            /* 设备不能播采样：退化成一次短震动，主机已经在震时不动。
             * 采样 ID 0x00 是「静音 / 停止播放」而不是一次播放——主机用它收掉
             * 「寻找手柄」的提示音，照脉冲处理会把马达一直留在震动上。 */
            strength = PAD_HAPTIC_PULSE;
        }
        out[off] = (uint8_t)((uint16_t)strength * max / 255u);
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
        ps_bt_frame(out, desc->len);
    }
    return desc->len;
}

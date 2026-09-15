#include "feedback.h"

#include <string.h>

/** NS2 输出报告：USB 形态的 0x02 是报告 ID + 左右 LRA 各 16 字节 + 9 字节保留。 */
#define PAD_NS2_OUT_REPORT_ID 0x02u
#define PAD_NS2_OUT_LRA_LEN 42u

/** 触觉采样退化成的短震动强度（0-255，按行内量程缩放后写入）。 */
#define PAD_HAPTIC_PULSE 0xC0u

static const pad_layout_t *s_last_layout;

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
        uint8_t strength = 0;
        if (feedback->rumble_on[side]) {
            strength = feedback->rumble_strength[side];
        } else if (feedback->haptic_sample_valid && desc->haptic == PAD_HAPTIC_AS_RUMBLE &&
                   !rumbling) {
            /* 设备不能播采样：退化成一次短震动，主机已经在震时不动。 */
            strength = PAD_HAPTIC_PULSE;
        }
        out[off] = (uint8_t)((uint16_t)strength * max / 255u);
    }

    if (desc->led_style != PAD_LED_NONE) {
        if (off_set(desc->led_mask_off) && desc->led_mask_off < desc->len) {
            out[desc->led_mask_off] = (uint8_t)(feedback->player_led & 0x0Fu);
        }
        if (desc->led_style == PAD_LED_LIGHTBAR && off_set(desc->led_rgb_off) &&
            (size_t)desc->led_rgb_off + 2u < desc->len) {
            led_color(feedback->player_led, &out[desc->led_rgb_off]);
        }
    }
    return desc->len;
}

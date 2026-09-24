#include "ns2_target.h"

#include <stddef.h>

#include "esp_log.h"

#include "ns2_output.h"
#include "ns2_state.h"

static const char *TAG = "remapad_ns2tgt";

/** NS2 目标消费的能力：震动可转发。 */
#define NS2_TARGET_CAPS (PAD_CAP_RUMBLE)

/** 扳机数字化阈值：NS2 只有数字 ZL/ZR，模拟扳机过半即按下。 */
#define NS2_TRIGGER_THRESHOLD ((PAD_AXIS_MAX + 1) / 2)

/** 私有按键位 → NS2 按键位：私有用 PS 键名、NS2 用 Nintendo 标签，
 *  两边按位置对齐（右→A、下→B、上→X、左→Y），不做二次重排。 */
static const struct {
    uint32_t pad;
    uint32_t ns2;
} s_button_map[] = {
    {PAD_BTN_CIRCLE, NS2_BTN_A},   /* ○ 右 → A 右 */
    {PAD_BTN_CROSS, NS2_BTN_B},    /* ✕ 下 → B 下 */
    {PAD_BTN_TRIANGLE, NS2_BTN_X}, /* △ 上 → X 上 */
    {PAD_BTN_SQUARE, NS2_BTN_Y},   /* □ 左 → Y 左 */
    {PAD_BTN_L1, NS2_BTN_L},
    {PAD_BTN_R1, NS2_BTN_R},
    {PAD_BTN_L3, NS2_BTN_LSTICK},
    {PAD_BTN_R3, NS2_BTN_RSTICK},
    {PAD_BTN_TOUCHPAD, NS2_BTN_MINUS}, /* 左侧小键（View / Select / SHARE / Create）→ 减号 */
    {PAD_BTN_OPT, NS2_BTN_PLUS},       /* Options / Menu → 加号 */
    {PAD_BTN_HOME, NS2_BTN_HOME},      /* PS 键 / 西瓜键 → Home */
    {PAD_BTN_SHARE, NS2_BTN_CAPTURE},  /* 分享类（触摸板按下 / Series 分享键）→ 截图 */
    {PAD_BTN_DPAD_UP, NS2_BTN_DPAD_UP},
    {PAD_BTN_DPAD_DOWN, NS2_BTN_DPAD_DOWN},
    {PAD_BTN_DPAD_LEFT, NS2_BTN_DPAD_LEFT},
    {PAD_BTN_DPAD_RIGHT, NS2_BTN_DPAD_RIGHT},
    /* 背键：NS2 只有 GL/GR 两个，四颗背键按侧合并。 */
    {PAD_BTN_L4, NS2_BTN_GL},
    {PAD_BTN_L5, NS2_BTN_GL},
    {PAD_BTN_R4, NS2_BTN_GR},
    {PAD_BTN_R5, NS2_BTN_GR},
    {PAD_BTN_MUTE, NS2_BTN_C},         /* 静音 → C 键 */
};

static pad_target_facts_t s_facts;
static uint32_t s_logged_unsupported;

static void ns2_set_facts(const pad_target_facts_t *facts)
{
    s_facts = *facts;
    /* 电池随报文上发，交给输出模块统一折进 0x05 / 0x09 的电源字段。 */
    ns2_output_set_battery(facts->battery_level, facts->battery_mv, facts->charging,
                           facts->external_power);
    /* 触觉特性开关决定 0x09 的状态标志字节，透传重写时也用它。 */
    ns2_output_set_rumble_enabled(facts->rumble_enabled);
}

static void ns2_from_pad(const pad_state_t *pad, ns2_controller_state_t *out)
{
    ns2_state_defaults(out);
    /* SHARE 与触摸板同帧双置：串流虚拟手柄（Sunshine/Moonlight）把一颗 View
     * 键双写成 SHARE+触摸板按下以兼容 PC 游戏，直译到 NS2 会让一次按键同时
     * 点亮减号与截图——按位置语义只保留减号。 */
    uint32_t buttons = pad->buttons;
    if ((buttons & (PAD_BTN_SHARE | PAD_BTN_TOUCHPAD)) == (PAD_BTN_SHARE | PAD_BTN_TOUCHPAD)) {
        buttons &= ~PAD_BTN_SHARE;
    }
    for (size_t i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
        if ((buttons & s_button_map[i].pad) != 0) {
            out->buttons |= s_button_map[i].ns2;
        }
    }
    if (pad->trigger[PAD_TRIGGER_L2] >= NS2_TRIGGER_THRESHOLD) {
        out->buttons |= NS2_BTN_ZL;
    }
    if (pad->trigger[PAD_TRIGGER_R2] >= NS2_TRIGGER_THRESHOLD) {
        out->buttons |= NS2_BTN_ZR;
    }
    out->stick_lx = pad->axis[PAD_AXIS_LX];
    out->stick_ly = pad->axis[PAD_AXIS_LY];
    out->stick_rx = pad->axis[PAD_AXIS_RX];
    out->stick_ry = pad->axis[PAD_AXIS_RY];
    out->battery_level = s_facts.battery_level;
    out->battery_mv = s_facts.battery_mv;
    out->charging = s_facts.charging;
    out->external_power = s_facts.external_power;
    out->rumble_enabled = s_facts.rumble_enabled;
    out->nfc_state = s_facts.nfc_state;
    /* 运动数据来自输入设备：透传路径不经过这里。 */
    out->motion_valid = pad->motion.present && (pad->caps & PAD_CAP_MOTION) != 0;
    for (size_t i = 0; i < 3; i++) {
        out->gyro[i] = pad->motion.gyro[i];
        out->accel[i] = pad->motion.accel[i];
    }
}

/** 私有 3.5mm 状态 → NS2 耳机状态字节：未插入 0x00、插入 0x05。
 *
 *  带麦那一档（0x07 / 0x0F）会被主机拒绝、输入不再被采用，因此不上行；
 *  要 A/B 时用串口 headset 钉一个值。 */
static uint8_t ns2_headset_state_from_pad(const pad_state_t *pad)
{
    if ((pad->caps & PAD_CAP_MIC) == 0u || !pad->headset_present) {
        return NS2_HEADSET_NONE;
    }
    return NS2_HEADSET_STEREO;
}

static void ns2_send_pad(const pad_state_t *pad)
{
    ns2_controller_state_t state;
    ns2_from_pad(pad, &state);
    /* 3.5mm 耳机状态：把派生值交给输出模块（覆盖值也在那里），编码路径与
     * 同代透传路径都从它取，0x09 与 0x05 因此不会各写一个值。 */
    ns2_output_set_headset_derived(ns2_headset_state_from_pad(pad));
    ns2_output_send(&state);

    /* 目标没有消费的能力位：只在集合变化时提示一次，避免每 5ms 刷日志。 */
    const uint32_t unsupported = pad->caps & ~NS2_TARGET_CAPS & ~PAD_CAP_FALLBACK_LAYOUT;
    if (unsupported != s_logged_unsupported) {
        s_logged_unsupported = unsupported;
        ESP_LOGI(TAG, "pad caps not consumed by ns2 target: 0x%02lx", (unsigned long)unsupported);
    }
}

/** 同代透传：NS2 手柄的报文体直接交给输出模块按会话匹配转发。 */
static bool ns2_send_raw(const pad_state_t *pad)
{
    return ns2_output_send_raw(pad);
}

static const pad_target_t s_ns2_target = {
    .name = "ns2",
    .caps = NS2_TARGET_CAPS,
    .language = PAD_LANG_NS2,
    .set_facts = ns2_set_facts,
    .send_pad = ns2_send_pad,
    .send_raw = ns2_send_raw,
};

const pad_target_t *ns2_target_get(void)
{
    return &s_ns2_target;
}

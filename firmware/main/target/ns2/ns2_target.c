#include "ns2_target.h"

#include <stddef.h>

#include "esp_log.h"

#include "ns2_output.h"
#include "ns2_state.h"

static const char *TAG = "remapad_ns2tgt";

/** NS2 目标支持的能力：震动可转发，运动数据与触摸板暂不在报文里。 */
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
    {PAD_BTN_LB, NS2_BTN_L},
    {PAD_BTN_RB, NS2_BTN_R},
    {PAD_BTN_LSTICK, NS2_BTN_LSTICK},
    {PAD_BTN_RSTICK, NS2_BTN_RSTICK},
    {PAD_BTN_BACK, NS2_BTN_MINUS},
    {PAD_BTN_START, NS2_BTN_PLUS},
    {PAD_BTN_GUIDE, NS2_BTN_HOME},
    {PAD_BTN_SHARE, NS2_BTN_CAPTURE},
    {PAD_BTN_DPAD_UP, NS2_BTN_DPAD_UP},
    {PAD_BTN_DPAD_DOWN, NS2_BTN_DPAD_DOWN},
    {PAD_BTN_DPAD_LEFT, NS2_BTN_DPAD_LEFT},
    {PAD_BTN_DPAD_RIGHT, NS2_BTN_DPAD_RIGHT},
    /* 背键：NS2 只有 GL/GR 两个，四颗背键按侧合并。 */
    {PAD_BTN_L4, NS2_BTN_GL},
    {PAD_BTN_L5, NS2_BTN_GL},
    {PAD_BTN_R4, NS2_BTN_GR},
    {PAD_BTN_R5, NS2_BTN_GR},
    {PAD_BTN_C, NS2_BTN_C},
};

static pad_target_facts_t s_facts;
static uint32_t s_logged_unsupported;

static void ns2_set_facts(const pad_target_facts_t *facts)
{
    s_facts = *facts;
    /* 电池随报文上发，交给输出模块统一折进 0x05 / 0x09 的电源字段。 */
    ns2_output_set_battery(facts->battery_level, facts->battery_mv, facts->charging,
                           facts->external_power);
}

static void ns2_from_pad(const pad_state_t *pad, ns2_controller_state_t *out)
{
    ns2_state_defaults(out);
    for (size_t i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
        if ((pad->buttons & s_button_map[i].pad) != 0) {
            out->buttons |= s_button_map[i].ns2;
        }
    }
    if (pad->trigger[PAD_TRIGGER_L] >= NS2_TRIGGER_THRESHOLD) {
        out->buttons |= NS2_BTN_ZL;
    }
    if (pad->trigger[PAD_TRIGGER_R] >= NS2_TRIGGER_THRESHOLD) {
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
}

static void ns2_send_pad(const pad_state_t *pad)
{
    ns2_controller_state_t state;
    ns2_from_pad(pad, &state);
    ns2_output_send(&state);

    /* 私有格式里有目标吃不下、本轮也不做映射的字段（IMU、触摸板、麦克风）。
     * 只在能力集合变化时提示一次，避免每 5ms 刷日志。 */
    const uint32_t unsupported = pad->caps & ~NS2_TARGET_CAPS & ~PAD_CAP_FALLBACK_LAYOUT;
    if (unsupported != s_logged_unsupported) {
        s_logged_unsupported = unsupported;
        ESP_LOGI(TAG, "pad caps not consumed by ns2 target: 0x%02lx", (unsigned long)unsupported);
    }
}

static const pad_target_t s_ns2_target = {
    .name = "ns2",
    .caps = NS2_TARGET_CAPS,
    .set_facts = ns2_set_facts,
    .send_pad = ns2_send_pad,
};

const pad_target_t *ns2_target_get(void)
{
    return &s_ns2_target;
}

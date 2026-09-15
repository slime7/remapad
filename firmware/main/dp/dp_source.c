#include "dp_source.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "dp_ui.h"

#define DP_SOURCE_MAX 4
#define DP_TICK_MS 5

static const char *TAG = "remapad_dp_src";

static const dp_source_t *s_sources[DP_SOURCE_MAX];
static size_t s_source_count;

/** 调试注入状态：bridge/控制面写入、dp_task 读取递减，临界区保护。 */
static portMUX_TYPE s_inject_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_inject_buttons;
static volatile uint32_t s_inject_hold_ticks;
/** 摇杆注入电平（LX, LY, RX, RY）：设定后持续生效，默认居中。 */
static volatile uint16_t s_inject_stick[PAD_AXIS_COUNT] = {
    PAD_AXIS_CENTER, PAD_AXIS_CENTER, PAD_AXIS_CENTER, PAD_AXIS_CENTER,
};
/** 已设定过的摇杆轴（bit0-3 对应 LX/LY/RX/RY）：未设定的轴沿用输入源的值。 */
static volatile uint8_t s_inject_stick_mask;

/** 调试按键名表：CLI 与用例共用，默认保持时长按操作节奏给（组合键更久）。 */
typedef struct {
    const char *name;
    uint32_t mask;
    uint32_t hold_ms;
} debug_key_t;

static const debug_key_t s_debug_keys[] = {
    /* 键名沿用 Nintendo 侧叫法（目标主机是 NS2），掩码写私有格式的键名：
       面键按 PS 的位置（a 在右、b 在下、x 在上、y 在左），plus 与 minus 走
       选项与触摸板位，home 与 capture 走主页与分享位，c 走静音位。 */
    {"a", PAD_BTN_CIRCLE, 250},
    {"b", PAD_BTN_CROSS, 250},
    {"x", PAD_BTN_TRIANGLE, 250},
    {"y", PAD_BTN_SQUARE, 250},
    {"plus", PAD_BTN_OPT, 250},
    {"minus", PAD_BTN_TOUCHPAD, 250},
    {"home", PAD_BTN_HOME, 250},
    {"capture", PAD_BTN_SHARE, 250},
    {"c", PAD_BTN_MUTE, 250},
    {"l", PAD_BTN_L1, 250},
    {"r", PAD_BTN_R1, 250},
    {"zl", PAD_BTN_L4, 250},
    {"zr", PAD_BTN_R4, 250},
    {"ls", PAD_BTN_L3, 250},
    {"rs", PAD_BTN_R3, 250},
    {"up", PAD_BTN_DPAD_UP, 250},
    {"down", PAD_BTN_DPAD_DOWN, 250},
    {"left", PAD_BTN_DPAD_LEFT, 250},
    {"right", PAD_BTN_DPAD_RIGHT, 250},
    {"gl", PAD_BTN_L4, 250},
    {"gr", PAD_BTN_R4, 250},
    /* 手柄操控 UI 的组合键：保持时长要盖过 dp_ui 的翻转阈值（300ms），
     * 注入一次就等价于按下组合键并松开，不插手柄也能验证整条 UI 操控链路。 */
    {"ui", DP_UI_COMBO_MASK, 500},
};

void dp_source_register(const dp_source_t *source)
{
    if (source == NULL || source->sample == NULL || s_source_count >= DP_SOURCE_MAX) {
        ESP_LOGE(TAG, "register failed (full=%u)", (unsigned)s_source_count);
        return;
    }
    s_sources[s_source_count++] = source;
    ESP_LOGI(TAG, "input source registered: %s (%u)", source->name, (unsigned)s_source_count);
}

/** 主输入源拥有除按键之外的全部字段：摇杆、扳机、触摸、运动与设备标识。 */
static void copy_primary_fields(pad_state_t *dst, const pad_state_t *src)
{
    memcpy(dst->axis, src->axis, sizeof(dst->axis));
    memcpy(dst->trigger, src->trigger, sizeof(dst->trigger));
    memcpy(dst->touch, src->touch, sizeof(dst->touch));
    dst->motion = src->motion;
    dst->mic_level = src->mic_level;
    dst->mic_muted = src->mic_muted;
    dst->battery_percent = src->battery_percent;
    dst->battery_present = src->battery_present;
    dst->charging = src->charging;
    dst->caps = src->caps;
    dst->family = src->family;
    dst->conn = src->conn;
    dst->vid = src->vid;
    dst->pid = src->pid;
    dst->report_id = src->report_id;
    dst->report_len = src->report_len;
    dst->seq = src->seq;
}

void dp_source_sample(pad_state_t *state)
{
    pad_state_defaults(state);
    for (size_t i = 0; i < s_source_count; i++) {
        pad_state_t part;
        pad_state_defaults(&part);
        s_sources[i]->sample(&part);
        if (i == 0) {
            copy_primary_fields(state, &part);
        }
        state->buttons |= part.buttons;
    }

    uint32_t inject = 0;
    uint16_t stick[PAD_AXIS_COUNT];
    uint8_t stick_mask = 0;
    portENTER_CRITICAL(&s_inject_mux);
    for (size_t i = 0; i < PAD_AXIS_COUNT; i++) {
        stick[i] = s_inject_stick[i];
    }
    stick_mask = s_inject_stick_mask;
    if (s_inject_hold_ticks > 0) {
        inject = s_inject_buttons;
        s_inject_hold_ticks--;
        if (s_inject_hold_ticks == 0) {
            s_inject_buttons = 0;
        }
    }
    portEXIT_CRITICAL(&s_inject_mux);
    /* 调试注入是最后叠加：按键叠加在合成按键上，设定过的摇杆轴覆盖合成值。 */
    state->buttons |= inject;
    for (size_t i = 0; i < PAD_AXIS_COUNT; i++) {
        if ((stick_mask & (1u << i)) != 0) {
            state->axis[i] = stick[i];
        }
    }
}

void dp_source_inject(uint32_t buttons_mask, uint32_t hold_ms)
{
    if (hold_ms < DP_TICK_MS) {
        hold_ms = DP_TICK_MS;
    }
    if (hold_ms > 60000) {
        hold_ms = 60000;
    }
    portENTER_CRITICAL(&s_inject_mux);
    s_inject_buttons |= buttons_mask;
    s_inject_hold_ticks = hold_ms / DP_TICK_MS;
    portEXIT_CRITICAL(&s_inject_mux);
    ESP_LOGI(TAG, "debug key inject: mask=0x%08lx hold=%lums",
             (unsigned long)buttons_mask, (unsigned long)hold_ms);
}

void dp_source_inject_release(void)
{
    portENTER_CRITICAL(&s_inject_mux);
    s_inject_buttons = 0;
    s_inject_hold_ticks = 0;
    portEXIT_CRITICAL(&s_inject_mux);
}

void dp_source_inject_stick(char side, uint16_t x, uint16_t y)
{
    const size_t base = side == 'r' ? PAD_AXIS_RX : PAD_AXIS_LX;
    if (x > PAD_AXIS_MAX) {
        x = PAD_AXIS_MAX;
    }
    if (y > PAD_AXIS_MAX) {
        y = PAD_AXIS_MAX;
    }
    portENTER_CRITICAL(&s_inject_mux);
    s_inject_stick[base] = x;
    s_inject_stick[base + 1] = y;
    s_inject_stick_mask |= (uint8_t)(0x03u << base);
    portEXIT_CRITICAL(&s_inject_mux);
    ESP_LOGI(TAG, "debug stick inject: %c x=%u y=%u",
             side == 'r' ? 'r' : 'l', (unsigned)x, (unsigned)y);
}

void dp_source_inject_stick_reset(void)
{
    portENTER_CRITICAL(&s_inject_mux);
    for (size_t i = 0; i < PAD_AXIS_COUNT; i++) {
        s_inject_stick[i] = PAD_AXIS_CENTER;
    }
    s_inject_stick_mask = 0;
    portEXIT_CRITICAL(&s_inject_mux);
    ESP_LOGI(TAG, "debug stick inject: reset");
}

bool dp_source_key_lookup(const char *name, size_t len, uint32_t *mask, uint32_t *hold_ms)
{
    if (name == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_debug_keys) / sizeof(s_debug_keys[0]); i++) {
        const debug_key_t *key = &s_debug_keys[i];
        if (strlen(key->name) == len && strncmp(key->name, name, len) == 0) {
            *mask = key->mask;
            *hold_ms = key->hold_ms;
            return true;
        }
    }
    return false;
}

bool dp_source_inject_active(void)
{
    return s_inject_hold_ticks > 0;
}

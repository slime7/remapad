#include "dp_source.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "ns2_output.h"

#define DP_SOURCE_MAX 4
#define DP_TICK_MS 5

static const char *TAG = "remapad_dp_src";

static const dp_source_t *s_sources[DP_SOURCE_MAX];
static size_t s_source_count;

/** 调试注入状态：bridge/控制面写入、dp_task 读取递减，临界区保护。 */
static portMUX_TYPE s_inject_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_inject_buttons;
static volatile uint32_t s_inject_hold_ticks;
/** 摇杆注入电平（lx, ly, rx, ry）：设定后持续生效，默认居中。 */
static volatile uint16_t s_inject_stick[4] = {
    NS2_STICK_CENTER, NS2_STICK_CENTER, NS2_STICK_CENTER, NS2_STICK_CENTER,
};
/** 已设定过的摇杆轴（bit0-3 对应 lx/ly/rx/ry）：未设定的轴沿用输入源的值。 */
static volatile uint8_t s_inject_stick_mask;

/** 调试按键名表：CLI 与用例共用，默认保持时长按操作节奏给（组合键更久）。 */
typedef struct {
    const char *name;
    uint32_t mask;
    uint32_t hold_ms;
} debug_key_t;

static const debug_key_t s_debug_keys[] = {
    {"a", NS2_BTN_A, 250},
    {"b", NS2_BTN_B, 250},
    {"x", NS2_BTN_X, 250},
    {"y", NS2_BTN_Y, 250},
    {"plus", NS2_BTN_PLUS, 250},
    {"minus", NS2_BTN_MINUS, 250},
    {"home", NS2_BTN_HOME, 250},
    {"capture", NS2_BTN_CAPTURE, 250},
    {"c", NS2_BTN_C, 250},
    {"l", NS2_BTN_L, 250},
    {"r", NS2_BTN_R, 250},
    {"zl", NS2_BTN_ZL, 250},
    {"zr", NS2_BTN_ZR, 250},
    {"ls", NS2_BTN_LSTICK, 250},
    {"rs", NS2_BTN_RSTICK, 250},
    {"up", NS2_BTN_DPAD_UP, 250},
    {"down", NS2_BTN_DPAD_DOWN, 250},
    {"left", NS2_BTN_DPAD_LEFT, 250},
    {"right", NS2_BTN_DPAD_RIGHT, 250},
    {"gl", NS2_BTN_GL, 250},
    {"gr", NS2_BTN_GR, 250},
    {"lr", NS2_BTN_L | NS2_BTN_R, 1000},
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

void dp_source_sample(ns2_controller_state_t *state)
{
    ns2_state_defaults(state);
    for (size_t i = 0; i < s_source_count; i++) {
        ns2_controller_state_t part;
        ns2_state_defaults(&part);
        s_sources[i]->sample(&part);
        state->buttons |= part.buttons;
        if (i == 0) {
            /* 主输入源拥有摇杆、电池与电源字段；后续源只叠加按键。 */
            state->stick_lx = part.stick_lx;
            state->stick_ly = part.stick_ly;
            state->stick_rx = part.stick_rx;
            state->stick_ry = part.stick_ry;
            state->battery_level = part.battery_level;
            state->battery_mv = part.battery_mv;
            state->external_power = part.external_power;
            state->charging = part.charging;
            state->fully_charged = part.fully_charged;
            state->rumble_enabled = part.rumble_enabled;
        }
    }
    state->nfc_state = ns2_output_nfc_state();

    uint32_t inject = 0;
    uint16_t stick[4];
    uint8_t stick_mask = 0;
    portENTER_CRITICAL(&s_inject_mux);
    stick[0] = s_inject_stick[0];
    stick[1] = s_inject_stick[1];
    stick[2] = s_inject_stick[2];
    stick[3] = s_inject_stick[3];
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
    if ((stick_mask & 0x01u) != 0) {
        state->stick_lx = stick[0];
    }
    if ((stick_mask & 0x02u) != 0) {
        state->stick_ly = stick[1];
    }
    if ((stick_mask & 0x04u) != 0) {
        state->stick_rx = stick[2];
    }
    if ((stick_mask & 0x08u) != 0) {
        state->stick_ry = stick[3];
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
    const size_t base = side == 'r' ? 2 : 0;
    if (x > NS2_STICK_MAX) {
        x = NS2_STICK_MAX;
    }
    if (y > NS2_STICK_MAX) {
        y = NS2_STICK_MAX;
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
    for (size_t i = 0; i < 4; i++) {
        s_inject_stick[i] = NS2_STICK_CENTER;
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

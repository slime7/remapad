#include "dp_source.h"

#include <stdbool.h>
#include <stdint.h>

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
    portENTER_CRITICAL(&s_inject_mux);
    if (s_inject_hold_ticks > 0) {
        inject = s_inject_buttons;
        s_inject_hold_ticks--;
        if (s_inject_hold_ticks == 0) {
            s_inject_buttons = 0;
        }
    }
    portEXIT_CRITICAL(&s_inject_mux);
    state->buttons |= inject;
}

void dp_source_inject(uint32_t buttons_mask, uint32_t hold_ms)
{
    if (hold_ms < DP_TICK_MS) {
        hold_ms = DP_TICK_MS;
    }
    if (hold_ms > 5000) {
        hold_ms = 5000;
    }
    portENTER_CRITICAL(&s_inject_mux);
    s_inject_buttons |= buttons_mask;
    s_inject_hold_ticks = hold_ms / DP_TICK_MS;
    portEXIT_CRITICAL(&s_inject_mux);
    ESP_LOGI(TAG, "debug key inject: mask=0x%08lx hold=%lums",
             (unsigned long)buttons_mask, (unsigned long)hold_ms);
}

bool dp_source_inject_active(void)
{
    return s_inject_hold_ticks > 0;
}

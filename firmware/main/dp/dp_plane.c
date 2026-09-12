#include "dp_plane.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_controller.h"
#include "ble_creds.h"
#include "ble_session.h"
#include "ns2_report.h"
#include "ns2_state.h"

static const char *TAG = "remapad_dp";

/** 调试注入状态：bridge（owner task）写入、dp_task 读取递减，临界区保护。 */
static portMUX_TYPE s_debug_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_debug_buttons;
static volatile uint32_t s_debug_hold_ticks;

#define DP_TICK_MS 5

/** 合成输入源：仅提供静置状态（摇杆居中、无按键）与电源/特性字段，
 *  按键输入全部来自调试注入，主机侧不应出现任何自动变化。 */
static void synthetic_sample(ns2_controller_state_t *state)
{
    ns2_state_defaults(state);
    state->battery_level = 8;
    state->battery_mv = 4120;
    state->rumble_enabled = ns2_session_rumble_enabled();
}

static void dp_task(void *param)
{
    ns2_controller_state_t state;
    uint8_t counter09 = 0;
    uint32_t counter05 = 0;
    TickType_t wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "data plane task running, tick=%dms, source=synthetic", DP_TICK_MS);
    for (;;) {
        synthetic_sample(&state);
        uint32_t debug_buttons = 0;
        portENTER_CRITICAL(&s_debug_mux);
        if (s_debug_hold_ticks > 0) {
            debug_buttons = s_debug_buttons;
            s_debug_hold_ticks--;
            if (s_debug_hold_ticks == 0) {
                s_debug_buttons = 0;
            }
        }
        portEXIT_CRITICAL(&s_debug_mux);
        state.buttons |= debug_buttons;
        if (ble_controller_connected()) {
            const uint8_t format = ns2_session_report_format();
            if (ble_controller_input_notify_ready(format)) {
                if (format == NS2_REPORT_ID_05) {
                    uint8_t report[NS2_INPUT_05_LEN];
                    ns2_encode_input_05(report, &state, counter05++);
                    ble_controller_notify_input_05(report);
                } else {
                    uint8_t report[NS2_INPUT_09_LEN];
                    ns2_encode_input_09(report, &state, counter09++);
                    ble_controller_notify_input_09(report);
                }
            }
        }
        /* vTaskDelayUntil 内部自行推进 wake；再手动累加会把实际周期翻倍。 */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(DP_TICK_MS));
    }
}

void dp_plane_debug_key(uint32_t buttons_mask, uint32_t hold_ms)
{
    if (hold_ms < DP_TICK_MS) {
        hold_ms = DP_TICK_MS;
    }
    if (hold_ms > 5000) {
        hold_ms = 5000;
    }
    portENTER_CRITICAL(&s_debug_mux);
    s_debug_buttons |= buttons_mask;
    s_debug_hold_ticks = hold_ms / DP_TICK_MS;
    portEXIT_CRITICAL(&s_debug_mux);
    ESP_LOGI(TAG, "debug key inject: mask=0x%08lx hold=%lums", (unsigned long)buttons_mask,
             (unsigned long)hold_ms);
}

esp_err_t dp_plane_start(void)
{
    /* 凭证装载须在 host 同步（决定回连/发现广播）之前完成。 */
    ble_creds_init();
    const esp_err_t err = ble_controller_start();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(dp_task, "remapad-dp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

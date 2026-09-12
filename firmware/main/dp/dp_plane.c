#include "dp_plane.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_controller.h"
#include "ble_session.h"
#include "ns2_report.h"
#include "ns2_state.h"

static const char *TAG = "remapad_dp";

#define DP_TICK_MS 5
#define DP_WALK_LEN 12
#define DP_WALK_PERIOD_TICKS 100

/** 合成输入源的按键遍历序列：每 500ms 前进一个，按下保持 250ms。 */
static const uint32_t s_walk[DP_WALK_LEN] = {
    NS2_BTN_A, NS2_BTN_B, NS2_BTN_X, NS2_BTN_Y,
    NS2_BTN_PLUS, NS2_BTN_MINUS, NS2_BTN_L, NS2_BTN_R,
    NS2_BTN_ZL, NS2_BTN_ZR, NS2_BTN_DPAD_UP, NS2_BTN_HOME,
};

/** 左摇杆 12 方向圆周（相对中心的偏移量，x113 后约 ±1100）。 */
static const int8_t s_circle[12][2] = {
    {0, -100}, {50, -87}, {87, -50}, {100, 0},
    {87, 50}, {50, 87}, {0, 100}, {-50, 87},
    {-87, 50}, {-100, 0}, {-87, -50}, {-50, -87},
};

static void synthetic_sample(ns2_controller_state_t *state, uint32_t tick)
{
    ns2_state_defaults(state);
    const uint32_t idx = (tick / DP_WALK_PERIOD_TICKS) % DP_WALK_LEN;
    if ((tick % DP_WALK_PERIOD_TICKS) < DP_WALK_PERIOD_TICKS / 2) {
        state->buttons |= s_walk[idx];
    }
    const int8_t *d = s_circle[(tick / 8) % 12];
    state->stick_lx = (uint16_t)(NS2_STICK_CENTER + d[0] * 11);
    state->stick_ly = (uint16_t)(NS2_STICK_CENTER + d[1] * 11);
    state->battery_level = 8;
    state->battery_mv = 4120;
    state->rumble_enabled = ns2_session_rumble_enabled();
}

static void dp_task(void *param)
{
    ns2_controller_state_t state;
    uint8_t counter09 = 0;
    uint32_t counter05 = 0;
    uint32_t tick = 0;
    TickType_t wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "data plane task running, tick=%dms, source=synthetic", DP_TICK_MS);
    for (;;) {
        synthetic_sample(&state, tick++);
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
        wake += pdMS_TO_TICKS(DP_TICK_MS);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(DP_TICK_MS));
    }
}

esp_err_t dp_plane_start(void)
{
    const esp_err_t err = ble_controller_start();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(dp_task, "remapad-dp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

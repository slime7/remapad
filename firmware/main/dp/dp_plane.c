#include "dp_plane.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "battery_curve.h"
#include "ble_controller.h"
#include "ble_creds.h"
#include "ble_session.h"
#include "dp_source.h"
#include "ns2_output.h"
#include "ns2_report.h"
#include "ns2_state.h"

static const char *TAG = "remapad_dp";

#define DP_TICK_MS 5

/** 合成输入源：仅提供静置状态（摇杆居中、无按键），电源/特性字段取自
 *  会话与电池驱动；按键输入全部来自调试注入，主机侧不应出现任何自动
 *  变化。M5 的 USB host 手柄源按 dp_source_t 再注册一路。 */
static void synthetic_sample(ns2_controller_state_t *state)
{
    /* 电量与端电压取自电池驱动；充电状态是趋势推断值（板上没有充电状态
     * 引脚），推断到充电即认为接了外部供电。 */
    const bool charging = battery_is_charging();
    state->battery_level = battery_ns2_level_from_percent(battery_get_percentage());
    state->battery_mv = (uint16_t)battery_get_voltage_mv();
    state->charging = charging;
    state->external_power = charging;
    state->rumble_enabled = ns2_session_rumble_enabled();
}

static const dp_source_t s_synthetic_source = {
    .name = "synthetic",
    .sample = synthetic_sample,
};

/** BLE 输出通道：把编码好的报告体经 NimBLE 通知发到对应连接。 */
static void ble_send_report(size_t index, uint8_t report_id, const uint8_t *body,
                            size_t len, void *user)
{
    (void)user;
    (void)len;
    ns2_session_deliver_report(index, report_id, body);
}

static size_t ble_session_count(void *user)
{
    (void)user;
    return ns2_session_output_count();
}

static bool ble_session_info(size_t index, uint8_t *identity, uint8_t *report_format,
                             void *user)
{
    (void)user;
    return ns2_session_output_info(index, identity, report_format);
}

static const ns2_output_sink_t s_ble_sink = {
    .session_count = ble_session_count,
    .session_info = ble_session_info,
    .send_report = ble_send_report,
    .user = NULL,
};

/** 主机反馈监听：结构化事件当前记录日志；M5 的 USB OUT / 桥接转发在此
 *  按目标设备编码后下发。 */
static void feedback_listener(ns2_feedback_type_t type, const void *payload, void *user)
{
    (void)user;
    switch (type) {
    case NS2_FEEDBACK_RUMBLE: {
        const ns2_rumble_event_t *rumble = payload;
        ESP_LOGI(TAG, "feedback rumble: L=%u R=%u (forward target pending M5)",
                 (unsigned)rumble->left_on, (unsigned)rumble->right_on);
        break;
    }
    case NS2_FEEDBACK_PLAYER_LED:
        ESP_LOGI(TAG, "feedback player LED 0x%x", *(const uint8_t *)payload);
        break;
    case NS2_FEEDBACK_HAPTIC_SAMPLE:
        ESP_LOGI(TAG, "feedback haptic sample 0x%02x", *(const uint8_t *)payload);
        break;
    default:
        break;
    }
}

static void dp_task(void *param)
{
    ns2_controller_state_t state;
    TickType_t wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "data plane task running, tick=%dms, source=synthetic+inject", DP_TICK_MS);
    for (;;) {
        dp_source_sample(&state);
        ns2_output_send(&state);
        /* vTaskDelayUntil 内部自行推进 wake；再手动累加会把实际周期翻倍。 */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(DP_TICK_MS));
    }
}

void dp_plane_debug_key(uint32_t buttons_mask, uint32_t hold_ms)
{
    dp_source_inject(buttons_mask, hold_ms);
}

esp_err_t dp_plane_start(void)
{
    /* 凭证装载须在 host 同步（决定回连/发现广播）之前完成。 */
    ble_creds_init();
    const esp_err_t err = ble_controller_start();
    if (err != ESP_OK) {
        return err;
    }
    battery_init();
    dp_source_register(&s_synthetic_source);
    ns2_output_set_sink(&s_ble_sink);
    ns2_output_set_feedback_listener(feedback_listener, NULL);
    if (xTaskCreate(dp_task, "remapad-dp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

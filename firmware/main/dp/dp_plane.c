#include "dp_plane.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "battery_curve.h"
#include "ble_controller.h"
#include "ble_creds.h"
#include "ble_session.h"
#include "dp_source.h"
#include "input_link.h"
#include "input_source.h"
#include "ns2_output.h"
#include "ns2_target.h"
#include "target.h"

static const char *TAG = "remapad_dp";

#define DP_TICK_MS 5

/** 目标上报分频：dp 每 5ms 采样，NS2 目标每 15ms 发一份报告（对齐已验证
 *  实现的 HID_REPORT_INTERVAL=15ms）。5ms 一发会超出链路吞吐：订阅后
 *  200Hz 的 63B 通知近半数因发送队列拥塞被丢，报文计数器跳号，主机拿到
 *  残缺流后不采用输入（实测「全要素正常但按键无反应」）。 */
#define DP_SEND_DIV 3

/** 按键变化日志的最小间隔：调试注入与桥接输入都在这一条里可见，
 *  限频后连点也不会刷屏（真机排查时按时间对得上串口日志）。 */
#define DP_BUTTON_LOG_MIN_INTERVAL_US (200 * 1000LL)

/** 合成输入源：只提供静置状态（摇杆居中、无按键）。板卡与主机会话侧的事实
 *  （电池、触觉特性、amiibo 状态）经目标侧刷新，按键输入来自调试注入与
 *  桥接 PC（input/ 注册的源）。 */
static void synthetic_sample(pad_state_t *state)
{
    (void)state;
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

/** 目标侧事实：电量与端电压取自电池驱动（充电状态是趋势推断值，板上没有
 *  充电状态引脚，推断到充电即认为接了外部供电）；触觉特性与 NFC 状态来自
 *  主机会话与 amiibo 预置。 */
static void refresh_target_facts(void)
{
    const bool charging = battery_is_charging();
    const pad_target_facts_t facts = {
        .battery_level = battery_ns2_level_from_percent(battery_get_percentage()),
        .battery_mv = (uint16_t)battery_get_voltage_mv(),
        .charging = charging,
        .external_power = charging,
        .rumble_enabled = ns2_session_rumble_enabled(),
        .nfc_state = ns2_output_nfc_state(),
    };
    target_set_facts(&facts);
}

/** 主机反馈监听：先归一到私有反馈格式（pad_feedback_t），投递路径（USB OUT /
 *  桥接回发）在后续里程碑实现，本轮只记录日志。 */
static void feedback_listener(ns2_feedback_type_t type, const void *payload, void *user)
{
    (void)user;
    pad_feedback_t feedback;
    pad_feedback_defaults(&feedback);
    switch (type) {
    case NS2_FEEDBACK_RUMBLE: {
        const ns2_rumble_event_t *rumble = payload;
        feedback.rumble_on[PAD_TRIGGER_L] = rumble->left_on;
        feedback.rumble_on[PAD_TRIGGER_R] = rumble->right_on;
        memcpy(feedback.rumble_raw[PAD_TRIGGER_L], rumble->raw, 16);
        memcpy(feedback.rumble_raw[PAD_TRIGGER_R], &rumble->raw[16], 16);
        ESP_LOGI(TAG, "feedback rumble: L=%u R=%u (delivery pending)",
                 (unsigned)rumble->left_on, (unsigned)rumble->right_on);
        break;
    }
    case NS2_FEEDBACK_PLAYER_LED:
        feedback.player_led = *(const uint8_t *)payload;
        ESP_LOGI(TAG, "feedback player LED 0x%x", feedback.player_led);
        break;
    case NS2_FEEDBACK_HAPTIC_SAMPLE:
        feedback.haptic_sample_valid = true;
        feedback.haptic_sample = *(const uint8_t *)payload;
        ESP_LOGI(TAG, "feedback haptic sample 0x%02x", feedback.haptic_sample);
        break;
    default:
        break;
    }
    /* 反向链路：主机反馈经桥接帧回发给 PC（投递到手柄在后续里程碑实现）。 */
    input_link_send_feedback(&feedback);
}

static void dp_task(void *param)
{
    (void)param;
    pad_state_t pad;
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_buttons = 0;
    int64_t last_button_log_us = 0;
    uint32_t send_div = 0;

    ESP_LOGI(TAG, "data plane task running, tick=%dms, target=%s", DP_TICK_MS,
             target_name());
    for (;;) {
        dp_source_sample(&pad);
        if (pad.buttons != last_buttons) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_button_log_us >= DP_BUTTON_LOG_MIN_INTERVAL_US) {
                ESP_LOGI(TAG, "buttons 0x%08lx -> 0x%08lx", (unsigned long)last_buttons,
                         (unsigned long)pad.buttons);
                last_button_log_us = now_us;
            }
            last_buttons = pad.buttons;
        }
        refresh_target_facts();
        if (++send_div >= DP_SEND_DIV) {
            send_div = 0;
            target_send_pad(&pad);
        }
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
    /* 目标：NS2（Pro Controller 2 与 JoyCon 2 共用一份编码实现）。将来支持
     * NS1 时在 target/ns1/ 新增实现并在这里切换。 */
    target_set(ns2_target_get());
    /* 输入源注册顺序即优先级：桥接 PC 先注册（拥有摇杆与设备字段），合成源
     * 只补静置状态，调试注入最后叠加。 */
    input_source_register();
    dp_source_register(&s_synthetic_source);
    ns2_output_set_sink(&s_ble_sink);
    ns2_output_set_feedback_listener(feedback_listener, NULL);
    if (xTaskCreate(dp_task, "remapad-dp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
#include "dp_ui.h"
#include "feedback.h"
#include "input_link.h"
#include "input_source.h"
#include "ns2_output.h"
#include "ns2_target.h"
#include "target.h"
#include "usb_input.h"

static const char *TAG = "remapad_dp";

#define DP_TICK_MS 5

/** 目标上报节奏：5 ms 采样、每 15 ms 发一份报告（ADR 0023 的取值）。
 *
 *  主机在初始化末尾用报告率描述符（0x0010 写 `85 00`）点的就是这一量级，
 *  2026-09-15 的实机对照确认只有它能被稳定吃下：15 ms 下 66.7 帧/秒、
 *  发送失败计数为 0；改成 5 ms 后发送失败与已发计数一起涨（四成以上通知因
 *  mbuf 耗尽被丢），有效投递反掉到 20 次/秒上下，主机侧表现为操作延迟与
 *  震动丢失。节奏因此写死，不再提供运行时档位。 */
#define DP_REPORT_INTERVAL_MS 15u

/** 上报分频：每 DP_TICK_MS 一次采样，够这个数就发一份报告。 */
#define DP_SEND_DIV (DP_REPORT_INTERVAL_MS / DP_TICK_MS)

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

/** 主机反馈的持续帧：BLE 回调只更新这一帧，编码与投递由数据面任务做。 */
static portMUX_TYPE s_feedback_mux = portMUX_INITIALIZER_UNLOCKED;
static pad_feedback_t s_feedback;
static volatile bool s_feedback_pending;

/**
 * 主机反馈监听：事件叠加进持续帧（pad_feedback_apply：事件带哪些字段就覆盖
 * 哪些字段，其余沿用上一帧），随后由数据面任务按设备布局编码并投递，绝不在
 * BLE 回调里碰 USB 传输。持续帧是必需的——每个事件都从默认值重建，会把刚
 * 点亮的玩家灯被随后的震动帧写灭，马达强度也在主机不更新时来回跳。
 */
/** 叠加进持续帧并标记待投递；PC 侧收叠加后的状态（打印与对账用）。 */
static void feedback_commit(uint8_t fields, const pad_feedback_t *event)
{
    pad_feedback_t merged;
    portENTER_CRITICAL(&s_feedback_mux);
    pad_feedback_apply(&s_feedback, fields, event);
    s_feedback_pending = true;
    merged = s_feedback;
    portEXIT_CRITICAL(&s_feedback_mux);
    input_link_send_feedback(&merged);
}

static void feedback_listener(ns2_feedback_type_t type, const void *payload, void *user)
{
    (void)user;
    pad_feedback_t event;
    pad_feedback_defaults(&event);
    uint8_t fields = 0;
    switch (type) {
    case NS2_FEEDBACK_RUMBLE: {
        const ns2_rumble_event_t *rumble = payload;
        event.rumble_on[PAD_TRIGGER_L2] = rumble->left_on;
        event.rumble_on[PAD_TRIGGER_R2] = rumble->right_on;
        event.rumble_strength[PAD_TRIGGER_L2] =
            rumble->left_on ? ns2_rumble_strength(rumble->raw) : 0;
        event.rumble_strength[PAD_TRIGGER_R2] =
            rumble->right_on ? ns2_rumble_strength(&rumble->raw[16]) : 0;
        memcpy(event.rumble_raw[PAD_TRIGGER_L2], rumble->raw, 16);
        memcpy(event.rumble_raw[PAD_TRIGGER_R2], &rumble->raw[16], 16);
        fields = PAD_FEEDBACK_FIELD_RUMBLE;
        ESP_LOGD(TAG, "feedback rumble: L=%u/%u R=%u/%u", (unsigned)rumble->left_on,
                 (unsigned)event.rumble_strength[PAD_TRIGGER_L2], (unsigned)rumble->right_on,
                 (unsigned)event.rumble_strength[PAD_TRIGGER_R2]);
        break;
    }
    case NS2_FEEDBACK_PLAYER_LED:
        event.player_led = *(const uint8_t *)payload;
        fields = PAD_FEEDBACK_FIELD_PLAYER_LED;
        ESP_LOGD(TAG, "feedback player LED 0x%x", event.player_led);
        break;
    case NS2_FEEDBACK_HAPTIC_SAMPLE:
        event.haptic_sample_valid = true;
        event.haptic_sample = *(const uint8_t *)payload;
        fields = PAD_FEEDBACK_FIELD_HAPTIC;
        ESP_LOGD(TAG, "feedback haptic sample 0x%02x", event.haptic_sample);
        break;
    default:
        break;
    }
    if (fields != 0) {
        feedback_commit(fields, &event);
    }
}

void dp_plane_inject_feedback(uint8_t fields, const pad_feedback_t *event)
{
    if (fields == 0 || event == NULL) {
        return;
    }
    feedback_commit(fields, event);
}

void dp_plane_feedback_held(pad_feedback_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_feedback_mux);
    *out = s_feedback;
    portEXIT_CRITICAL(&s_feedback_mux);
}

/**
 * 反馈投递：按来源设备的布局把反馈编码成该设备的输出报告。USB host 直插
 * 走 OUT 端点，桥接路径把原始输出报告交给 PC（PC 只负责写手柄）。
 */
static void deliver_feedback(const pad_feedback_t *feedback)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    pad_conn_t conn = PAD_CONN_UNKNOWN;
    const bool usb = usb_input_device_ids(&vid, &pid, &conn);
    if (!usb && !input_source_device_ids(&vid, &pid, &conn)) {
        return;
    }
    uint8_t out[PAD_OUTPUT_MAX];
    const size_t len = pad_feedback_encode(conn, vid, pid, feedback, out, sizeof(out));
    if (len == 0) {
        return;
    }
    const pad_layout_t *layout = pad_feedback_last_layout();
    ESP_LOGD(TAG, "feedback -> %04x:%04x %u bytes (%s)", (unsigned)vid, (unsigned)pid,
             (unsigned)len, layout != NULL ? pad_family_name(layout->family) : "-");
    if (usb) {
        usb_input_send_output(out, len);
    }
    if (input_source_attached()) {
        input_link_send_out_report(out, len);
    }
}

/**
 * 一帧「全部松开」的中性报文：捕获组合键的那一刻补发一份（主机的按键状态是
 * 最后一份报文的内容，正按着的键不会自己弹起来），捕获期间再按上报节奏持续
 * 续发——主机靠稳定不跳号的上报流判断链路健康，整段停发会让它把手柄判成
 * 离线。同代透传载荷一并清掉：原样转发的报文体里带着被捕获的那几个键，
 * 中性帧就成了白发。
 */
static void send_neutral_report(const pad_state_t *pad)
{
    pad_state_t neutral = *pad;
    neutral.buttons = 0;
    for (size_t axis = 0; axis < PAD_AXIS_COUNT; axis++) {
        neutral.axis[axis] = PAD_AXIS_CENTER;
    }
    for (size_t trigger = 0; trigger < PAD_TRIGGER_COUNT; trigger++) {
        neutral.trigger[trigger] = 0;
    }
    neutral.motion.present = false;
    neutral.raw_len = 0;
    neutral.native_lang = PAD_LANG_NONE;
    target_send_pad(&neutral);
}

static void dp_task(void *param)
{
    (void)param;
    pad_state_t pad;
    ns2_adv_home_key_t home_key = {0};
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_buttons = 0;
    int64_t last_button_log_us = 0;
    uint32_t send_div = 0;
    bool output_paused = false;

    ESP_LOGI(TAG, "data plane task running, tick=%dms, target=%s", DP_TICK_MS,
             target_name());
    for (;;) {
        dp_source_sample(&pad);
        /* 实体手柄的 HOME：主机不在线时它就是「唤醒手柄」键——开唤醒窗口发
         *  0x81，睡下的主机被叫醒后自动连回来、醒着的直接连回来；在线时 HOME
         *  照常作为主页键进报文。按下那一刻触发一次，按住不重复（见
         *  ns2_adv_home_key_step）。 */
        const bool home_pressed = (pad.buttons & PAD_BTN_HOME) != 0;
        if (ns2_adv_home_key_step(&home_key, home_pressed) &&
            ns2_adv_home_action(ble_controller_connected()) == NS2_HOME_WAKE) {
            ESP_LOGI(TAG, "home key: opening the wake window");
            ns2_session_wake_request();
        }
        if (s_feedback_pending) {
            pad_feedback_t feedback;
            portENTER_CRITICAL(&s_feedback_mux);
            feedback = s_feedback;
            s_feedback_pending = false;
            portEXIT_CRITICAL(&s_feedback_mux);
            deliver_feedback(&feedback);
        }
        /* 主机断开后没人再更新反馈：持续帧会把手柄悬在最后一次震动上（手柄自己
         * 不知道主机走了），断开时补一帧把震动与一次性采样清掉。 */
        if (!ble_controller_connected()) {
            bool stale = false;
            portENTER_CRITICAL(&s_feedback_mux);
            if (s_feedback.rumble_on[PAD_TRIGGER_L2] || s_feedback.rumble_on[PAD_TRIGGER_R2] ||
                s_feedback.haptic_sample_valid) {
                pad_feedback_t cleared;
                pad_feedback_defaults(&cleared);
                pad_feedback_apply(&s_feedback, PAD_FEEDBACK_FIELD_RUMBLE, &cleared);
                s_feedback_pending = true;
                stale = true;
            }
            portEXIT_CRITICAL(&s_feedback_mux);
            if (stale) {
                ESP_LOGD(TAG, "host gone: clearing held rumble state");
            }
        }
        if (pad.buttons != last_buttons) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_button_log_us >= DP_BUTTON_LOG_MIN_INTERVAL_US) {
                ESP_LOGI(TAG, "buttons 0x%08lx -> 0x%08lx", (unsigned long)last_buttons,
                         (unsigned long)pad.buttons);
                last_button_log_us = now_us;
            }
            last_buttons = pad.buttons;
        }
        const dp_ui_event_t ui_event = dp_ui_frame(pad.buttons, DP_TICK_MS);
        /* 捕获期间不上行玩家输入：组合键一按下就切换（不必等翻转），退出模式
         * 后若组合键还按着也保持到松开为止（见 dp_ui.h）。 */
        const bool paused = dp_ui_captured(pad.buttons) || dp_ui_active();
        if (paused != output_paused) {
            output_paused = paused;
            if (paused) {
                /* 切换的那一刻就补一帧全松开，不等下个上报节拍：被捕获时正按着
                 * 的键会一直按在主机那头。 */
                send_neutral_report(&pad);
            }
            ESP_LOGI(TAG, "pad output %s", paused ? "paused" : "resumed");
        }
        if (ui_event == DP_UI_EVENT_ENTERED) {
            ESP_LOGI(TAG, "pad captures the screen: dpad moves focus, circle confirms");
        }
        refresh_target_facts();
        if (++send_div >= DP_SEND_DIV) {
            send_div = 0;
            if (paused) {
                /* 捕获期间续发中性帧：主机按「稳定不跳号的上报流」判断链路
                 * 健康，整段停发会让它把手柄判成离线；玩家输入一点不上行。 */
                send_neutral_report(&pad);
            } else {
                target_send_pad(&pad);
            }
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
     * 只补静置状态，USB host 直插与桥接现实中互斥（同一个 Type-C），调试注入
     * 最后叠加。 */
    input_source_register();
    usb_input_register();
    dp_source_register(&s_synthetic_source);
    ns2_output_set_sink(&s_ble_sink);
    ns2_output_set_feedback_listener(feedback_listener, NULL);
    if (xTaskCreate(dp_task, "remapad-dp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

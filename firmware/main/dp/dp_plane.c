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

/** 采样脉冲的自灭时限：主机正常会用 0x00 采样收掉提示音，但忘了发或丢包时
 *  不能把马达钉在脉冲上；主机以十几 Hz 重发采样时脉冲自然续上。 */
#define DP_HAPTIC_HOLD_US 300000LL

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
 *  主机会话与 amiibo 预置。输入设备自报电量（PAD_CAP_BATTERY）时覆盖电源
 *  字段的电量档位——主机看到的电量跟手柄走，板载电池只是设备没报时的兜底。 */
static void refresh_target_facts(const pad_state_t *pad)
{
    const bool charging = battery_is_charging();
    pad_target_facts_t facts = {
        .battery_level = battery_ns2_level_from_percent(battery_get_percentage()),
        .battery_mv = (uint16_t)battery_get_voltage_mv(),
        .charging = charging,
        .external_power = charging,
        .rumble_enabled = ns2_session_rumble_enabled(),
        .nfc_state = ns2_output_nfc_state(),
    };
    target_apply_pad_battery(&facts, pad);
    target_set_facts(&facts);
}

/** 主机反馈的持续帧：BLE 回调只更新这一帧，编码与投递由数据面任务做。 */
static portMUX_TYPE s_feedback_mux = portMUX_INITIALIZER_UNLOCKED;

/** 最近一次触觉采样事件的时刻（feedback_commit 里刷新），采样脉冲的超时
 *  自灭按它算。 */
static volatile int64_t s_haptic_last_us;
/** 当前这段采样的起播时刻（effective 采样从无到有的那一刻），包络相位按它算。 */
static volatile int64_t s_haptic_start_us;
static pad_feedback_t s_feedback;
static volatile bool s_feedback_pending;

/** 桥接路径的音频触觉让位开关（PC 经 `haptic audio on|off` 告知）。 */
static volatile bool s_bridge_audio_haptics;

/**
 * 主机反馈监听：事件叠加进持续帧（pad_feedback_apply：事件带哪些字段就覆盖
 * 哪些字段，其余沿用上一帧），编码与投递由数据面任务做，绝不在 BLE 回调里碰
 * USB/串口传输——震动写入跑在 NimBLE 主机任务里，游戏内主机的震动流接近输入
 * 上报的频率，任何阻塞 IO 都会拖住输入通知（主机侧操作变卡）。持续帧是必需
 * 的——每个事件都从默认值重建，会把刚点亮的玩家灯被随后的震动帧写灭，马达
 * 强度也在主机不更新时来回跳。
 */
static void feedback_commit(uint8_t fields, const pad_feedback_t *event)
{
    /* 最近一次采样事件的时间戳：脉冲的超时自灭按它算（esp_timer 读寄存器，
     * 放临界区外读，先后差一拍不影响语义）。 */
    const int64_t haptic_now_us =
        (fields & PAD_FEEDBACK_FIELD_HAPTIC) != 0 ? esp_timer_get_time() : 0;
    portENTER_CRITICAL(&s_feedback_mux);
    const bool was_active =
        s_feedback.haptic_sample_valid && s_feedback.haptic_sample != 0;
    if ((fields & PAD_FEEDBACK_FIELD_HAPTIC) != 0) {
        s_haptic_last_us = haptic_now_us;
    }
    pad_feedback_apply(&s_feedback, fields, event);
    if (!was_active && s_feedback.haptic_sample_valid && s_feedback.haptic_sample != 0) {
        /* 起播时刻：包络相位从这一拍算起，重发同一采样只续命、不重启节奏。 */
        s_haptic_start_us = haptic_now_us;
    }
    s_feedback_pending = true;
    portEXIT_CRITICAL(&s_feedback_mux);
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
        uint8_t lf = 0;
        uint8_t hf = 0;
        uint16_t lf_hz = 0;
        uint16_t hf_hz = 0;
        event.rumble_on[PAD_TRIGGER_L2] = rumble->left_on;
        event.rumble_on[PAD_TRIGGER_R2] = rumble->right_on;
        ns2_rumble_band_strengths(rumble->raw, &lf, &hf);
        /* NS2 的线性档位直写 ERM 马达落在死区：归一时按感知曲线重映射，
         * 马达编码、板上合成与 PC 合成吃的都是这份值。 */
        lf = pad_rumble_perceived(lf);
        hf = pad_rumble_perceived(hf);
        event.rumble_strength[PAD_TRIGGER_L2] = rumble->left_on ? lf : 0;
        event.rumble_hf_strength[PAD_TRIGGER_L2] = rumble->left_on ? hf : 0;
        ns2_rumble_band_strengths(&rumble->raw[16], &lf, &hf);
        lf = pad_rumble_perceived(lf);
        hf = pad_rumble_perceived(hf);
        event.rumble_strength[PAD_TRIGGER_R2] = rumble->right_on ? lf : 0;
        event.rumble_hf_strength[PAD_TRIGGER_R2] = rumble->right_on ? hf : 0;
        /* 驱动频率的落地值（合成侧语义：0 回落缺省、越界夹取）：USB 音频触觉
         *  与桥接 FEEDBACK 帧共用这一份，PC 不重复落地规则。 */
        ns2_rumble_band_frequencies(rumble->raw, &lf_hz, &hf_hz);
        event.rumble_lf_freq[PAD_TRIGGER_L2] = haptic_synth_band_freq(lf_hz, false);
        event.rumble_hf_freq[PAD_TRIGGER_L2] = haptic_synth_band_freq(hf_hz, true);
        ns2_rumble_band_frequencies(&rumble->raw[16], &lf_hz, &hf_hz);
        event.rumble_lf_freq[PAD_TRIGGER_R2] = haptic_synth_band_freq(lf_hz, false);
        event.rumble_hf_freq[PAD_TRIGGER_R2] = haptic_synth_band_freq(hf_hz, true);
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

void dp_plane_bridge_audio_haptics(bool on)
{
    s_bridge_audio_haptics = on;
    /* 让位切换后立即补一帧：正震着的时候切开关，两侧写回要尽快对齐新语义。 */
    portENTER_CRITICAL(&s_feedback_mux);
    s_feedback_pending = true;
    portEXIT_CRITICAL(&s_feedback_mux);
    ESP_LOGI(TAG, "bridge audio haptics %s", on ? "on" : "off");
}

bool dp_plane_bridge_audio_active(void)
{
    return s_bridge_audio_haptics;
}

/**
 * 把持续帧按输入设备的布局编码成输出报告（USB host 直插与桥接共用这一份
 * 字节）。返回编码长度，设备没有反馈通道时返回 0。编码与发送分开：调用方
 * 要先拿编码字节做变化判定，再决定发不发。
 */
static size_t encode_feedback_report(const pad_feedback_t *feedback, uint8_t *out, size_t cap)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    pad_conn_t conn = PAD_CONN_UNKNOWN;
    if (!usb_input_device_ids(&vid, &pid, &conn) &&
        !input_source_device_ids(&vid, &pid, &conn)) {
        return 0;
    }
    return pad_feedback_encode(conn, vid, pid, feedback, out, cap);
}

/** 「震动让位」版本的输出报告：马达字节全零、玩家灯照常——音频触觉接手的
 *  那条通路拿它写回，同一对音圈不再被 HID 与音频双驱动。 */
static size_t encode_quiet_feedback(const pad_feedback_t *feedback, uint8_t *out, size_t cap)
{
    pad_feedback_t quiet;
    pad_feedback_defaults(&quiet);
    quiet.player_led = feedback->player_led;
    return encode_feedback_report(&quiet, out, cap);
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

/** 采样音色的当前渲染幅度：持续帧的采样有效且没到超时自灭才给值，否则 0。
 *  幅度只按音色表的段边界变化（几百毫秒一档），数据面据此步进重编码——这
 *  也是主机停止重发后超时自灭能走进编码路径的入口（持续帧自己不会再置
 *  pending）。 */
static uint8_t held_haptic_envelope(int64_t now_us)
{
    portENTER_CRITICAL(&s_feedback_mux);
    const bool active =
        s_feedback.haptic_sample_valid && s_feedback.haptic_sample != 0;
    const uint8_t sample = s_feedback.haptic_sample;
    const int64_t start = s_haptic_start_us;
    const int64_t last = s_haptic_last_us;
    portEXIT_CRITICAL(&s_feedback_mux);
    if (!active || now_us - last > DP_HAPTIC_HOLD_US) {
        return 0;
    }
    return pad_haptic_pulse_envelope(sample, (uint32_t)((now_us - start) / 1000));
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
    /* 上次发往桥接/USB 的反馈帧与编码报告：等价帧不再重复占传输（分别见
     * pad_feedback_equal 与下面的编码字节比较）。 */
    pad_feedback_t feedback_sent;
    pad_feedback_defaults(&feedback_sent);
    bool feedback_sent_valid = false;
    /* 上次编码用到的采样音色幅度：只在音色段边界变化，值变了才走一遍
     * 编码与写回（反馈事件与音色步进共用同一条路径）。 */
    uint8_t haptic_env_last = 0;
    uint8_t bridge_out_sent[PAD_OUTPUT_MAX];
    size_t bridge_out_sent_len = 0;
    bool bridge_out_valid = false;
    uint8_t usb_out_sent[PAD_OUTPUT_MAX];
    size_t usb_out_sent_len = 0;
    bool usb_out_valid = false;

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
        /* 采样音色的当前幅度：主机只重发采样 ID，播放形态由音色表给出；
         *  音色步进与反馈事件共用下面这一条编码与写回路径。 */
        const uint8_t haptic_env = held_haptic_envelope(esp_timer_get_time());
        if (s_feedback_pending || haptic_env != haptic_env_last) {
            haptic_env_last = haptic_env;
            pad_feedback_t feedback;
            portENTER_CRITICAL(&s_feedback_mux);
            feedback = s_feedback;
            s_feedback_pending = false;
            portEXIT_CRITICAL(&s_feedback_mux);
            /* 采样脉冲超时自灭：只在本地副本上清，不动持续帧——新采样事件
             *  会刷新时戳并重新触发。 */
            if (feedback.haptic_sample_valid &&
                esp_timer_get_time() - s_haptic_last_us > DP_HAPTIC_HOLD_US) {
                feedback.haptic_sample_valid = false;
                feedback.haptic_sample = 0;
            }
            /* 采样渲染副本：编码、写回与 FEEDBACK 帧吃的是音色渲染幅度
             *  （原始采样 ID 只表示「在播哪一档」），两条触觉通路因此跟着
             *  音色节奏走，而不是被恒定强度钉成「一直震」。 */
            pad_feedback_t render = feedback;
            const bool sample_active =
                feedback.haptic_sample_valid && feedback.haptic_sample != 0;
            if (sample_active) {
                render.haptic_sample = haptic_env;
            }
            /* 写回（USB OUT / 桥接 OUT_REPORT）按「编码后的报告字节变了才发」：
             *  主机的震动流是音频式连续包络，原始参数包逐包都在抖，但真正落到
             *  马达/灯字节的值常常几十包不变——只有字节变化的帧才值得占一次
             *  传输，PC 会话循环才腾得出手转发输入。同代透传（NS2 手柄）的
             *  参数包原样在编码字节里，逐包纹理照常透传。 */
            uint8_t feedback_out[PAD_OUTPUT_MAX];
            const size_t feedback_out_len =
                encode_feedback_report(&render, feedback_out, sizeof(feedback_out));
            const pad_layout_t *feedback_layout =
                feedback_out_len > 0 ? pad_feedback_last_layout() : NULL;

            /* 音频触觉让位（USB 直插走板上合成、桥接走 PC 侧合成）：两颗音圈
             *  被 HID 与音频同时驱动会叠成浑浊触感，接手的一条通路拿到的 HID
             *  报告把震动字段清零。频率落地值随持续帧走，两侧合成同一份数值
             *  （PC 只做哑渲染，落地规则只在固件里有一份）。 */
            const bool usb_audio_engaged = usb_input_attached() && feedback_layout != NULL &&
                                           feedback_layout->out.audio_haptic &&
                                           usb_input_audio_haptics();
            const bool bridge_audio_engaged = input_source_attached() && feedback_layout != NULL &&
                                              feedback_layout->out.audio_haptic &&
                                              s_bridge_audio_haptics;
            if (usb_audio_engaged) {
                haptic_synth_params_t haptic;
                memset(&haptic, 0, sizeof(haptic));
                for (size_t side = 0; side < PAD_TRIGGER_COUNT; side++) {
                    haptic.lf_amp[side] = feedback.rumble_strength[side];
                    haptic.hf_amp[side] = feedback.rumble_hf_strength[side];
                    haptic.lf_freq[side] = feedback.rumble_lf_freq[side];
                    haptic.hf_freq[side] = feedback.rumble_hf_freq[side];
                }
                haptic.pulse = sample_active ? haptic_env : 0;
                usb_input_haptic(&haptic);
            }

            /* USB 直插写回：让位时发清了震动字节的报告（玩家灯照常）。 */
            if (usb_input_attached() && feedback_out_len > 0) {
                uint8_t usb_out[PAD_OUTPUT_MAX];
                size_t usb_out_len = feedback_out_len;
                memcpy(usb_out, feedback_out, feedback_out_len);
                if (usb_audio_engaged) {
                    usb_out_len = encode_quiet_feedback(&render, usb_out, sizeof(usb_out));
                }
                if (usb_out_len > 0 &&
                    (!usb_out_valid || usb_out_len != usb_out_sent_len ||
                     memcmp(usb_out_sent, usb_out, usb_out_len) != 0)) {
                    ESP_LOGD(TAG, "feedback(usb) -> %u bytes%s", (unsigned)usb_out_len,
                             usb_audio_engaged ? " (haptics on audio)" : "");
                    usb_input_send_output(usb_out, usb_out_len);
                    memcpy(usb_out_sent, usb_out, usb_out_len);
                    usb_out_sent_len = usb_out_len;
                    usb_out_valid = true;
                }
            } else {
                usb_out_valid = false;
            }

            /* 桥接写回：PC 只负责把报告写给手柄；它自己接管音频触觉时拿让位
             *  版本，否则完整报告。 */
            if (input_source_attached() && feedback_out_len > 0) {
                uint8_t bridge_out[PAD_OUTPUT_MAX];
                size_t bridge_out_len = feedback_out_len;
                memcpy(bridge_out, feedback_out, feedback_out_len);
                if (bridge_audio_engaged) {
                    bridge_out_len = encode_quiet_feedback(&render, bridge_out,
                                                           sizeof(bridge_out));
                }
                if (bridge_out_len > 0 &&
                    (!bridge_out_valid || bridge_out_len != bridge_out_sent_len ||
                     memcmp(bridge_out_sent, bridge_out, bridge_out_len) != 0)) {
                    ESP_LOGD(TAG, "feedback(bridge) -> %u bytes (%s)%s",
                             (unsigned)bridge_out_len,
                             feedback_layout != NULL
                                 ? pad_family_name(feedback_layout->family)
                                 : "-",
                             bridge_audio_engaged ? " (haptics on pc audio)" : "");
                    input_link_send_out_report(bridge_out, bridge_out_len);
                    memcpy(bridge_out_sent, bridge_out, bridge_out_len);
                    bridge_out_sent_len = bridge_out_len;
                    bridge_out_valid = true;
                }
            }
            /* 桥接的反馈状态帧（PC 日志展示与音频触觉输入）按写回语义变化才发：
             *  原始字节的抖动不产生新帧，采样字节是包络渲染值（PC 只做哑渲染）。 */
            if (!feedback_sent_valid || !pad_feedback_equal(&feedback_sent, &render)) {
                input_link_send_feedback(&render);
                feedback_sent = render;
                feedback_sent_valid = true;
            }
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
                pad_feedback_apply(&s_feedback,
                                   PAD_FEEDBACK_FIELD_RUMBLE | PAD_FEEDBACK_FIELD_HAPTIC,
                                   &cleared);
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
        refresh_target_facts(&pad);
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
    /* 目标：NS2（设备只模拟 Pro Controller 2）。将来支持 NS1 时在
     * target/ns1/ 新增实现并在这里切换。 */
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

#include "dp_plane.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "battery.h"
#include "battery_curve.h"
#include "ble_controller.h"
#include "ble_creds.h"
#include "ble_session.h"
#include "buzzer.h"
#include "dp_capture.h"
#include "dp_source.h"
#include "dp_ui.h"
#include "ds_behavior.h"
#include "feedback.h"
#include "input_frame.h"
#include "input_link.h"
#include "input_source.h"
#include "ns2_output.h"
#include "ns2_target.h"
#include "target.h"
#include "usb_input.h"

static const char *TAG = "remapad_dp";

#define DP_TICK_MS 5

/** 采集播放的自灭时限：主机正常会用 0x00 采样收掉提示音，但忘了发或丢包时
 *  不能把蜂鸣钉在响声上；主机以十几 Hz 重发采样时节奏自然续上。 */
#define DP_HAPTIC_HOLD_US 300000LL

/** 单拍最多发几条采集帧：突发时余下的留在环里，别把一拍时间全交给串口。 */
#define DP_CAPTURE_DRAIN_MAX 8

/** 目标上报节奏：5 ms 采样、每 15 ms 发一份报告，写死不提供运行时档位。 */
#define DP_REPORT_INTERVAL_MS 15u

/** 上报分频：每 DP_TICK_MS 一次采样，够这个数就发一份报告。 */
#define DP_SEND_DIV (DP_REPORT_INTERVAL_MS / DP_TICK_MS)

/** 按键变化日志的最小间隔：调试注入与桥接输入都在这一条里可见，
 *  限频后连点也不会刷屏。 */
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
 * 主机反馈监听：事件叠加进持续帧（事件带哪些字段就覆盖哪些字段，其余沿用上一帧），
 * 编码与投递由数据面任务做，绝不在 BLE 回调里碰 USB/串口传输——阻塞 IO 会拖住输入通知。
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
        /* NS2 波形规则的完整形态（每侧时序子帧）随事件进持续帧：HD 触觉
         * 映射按它把主机的波形按时间顺序重整成目标设备的 PCM。 */
        ns2_rumble_key_t keys[PAD_RUMBLE_KEY_COUNT];
        event.rumble_key_count[PAD_TRIGGER_L2] =
            (uint8_t)ns2_rumble_keys(rumble->raw, keys);
        for (size_t k = 0; k < PAD_RUMBLE_KEY_COUNT; k++) {
            event.rumble_keys[PAD_TRIGGER_L2][k] = keys[k];
        }
        event.rumble_key_count[PAD_TRIGGER_R2] =
            (uint8_t)ns2_rumble_keys(&rumble->raw[16], keys);
        for (size_t k = 0; k < PAD_RUMBLE_KEY_COUNT; k++) {
            event.rumble_keys[PAD_TRIGGER_R2][k] = keys[k];
        }
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
static size_t encode_feedback_report_ex(const pad_feedback_t *feedback, uint8_t *out,
                                        size_t cap, bool quiet)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    pad_conn_t conn = PAD_CONN_UNKNOWN;
    if (!usb_input_device_ids(&vid, &pid, &conn) &&
        !input_source_device_ids(&vid, &pid, &conn)) {
        return 0;
    }
    if (quiet) {
        return pad_feedback_encode_quiet(conn, vid, pid, feedback, out, cap);
    }
    return pad_feedback_encode(conn, vid, pid, feedback, out, cap);
}

static size_t encode_feedback_report(const pad_feedback_t *feedback, uint8_t *out, size_t cap)
{
    return encode_feedback_report_ex(feedback, out, cap, false);
}

/** 「震动让位」版本的输出报告：马达字节全零、玩家灯照常——音频触觉接手的
 *  那条通路拿它写回，同一对音圈不再被 HID 与音频双驱动。编码用布局行的
 *  quiet_presets：震动位段换成「COMPATIBLE_VIBRATION 不带 HAPTICS_SELECT」，
 *  这是把手柄的音圈交还给音频触觉的那次切换——照抄完整预置的 HAPTICS_SELECT
 *  会让音圈停在震动仿真模式、触觉 PCM 被静音（让位后只剩玩家灯）。 */
static size_t encode_quiet_feedback(const pad_feedback_t *feedback, uint8_t *out, size_t cap)
{
    pad_feedback_t quiet;
    pad_feedback_defaults(&quiet);
    quiet.player_led = feedback->player_led;
    return encode_feedback_report_ex(&quiet, out, cap, true);
}

/**
 * 一帧「全部松开」的中性报文：捕获组合键那一刻补发一份（否则正按着的键停在主机侧），
 * 捕获期间按上报节奏续发，避免主机把停发判成离线；同代透传载荷一并清掉。
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

/** 采样音色的当前播放位：持续帧的采样有效且没到超时自灭才给值，否则 0。
 *  remain_ms（可空）带距下一段段边界的毫秒数，蜂鸣器按段定鸣叫时长；
 *  tone_hz（可空）带「发声」段的音高。幅度只按音色表的段边界变化（几百
 *  毫秒一档），蜂鸣的响/停跟着段走。 */
static uint8_t held_haptic_envelope(int64_t now_us, uint32_t *remain_ms, uint16_t *tone_hz)
{
    portENTER_CRITICAL(&s_feedback_mux);
    const bool active =
        s_feedback.haptic_sample_valid && s_feedback.haptic_sample != 0;
    const uint8_t sample = s_feedback.haptic_sample;
    const int64_t start = s_haptic_start_us;
    const int64_t last = s_haptic_last_us;
    portEXIT_CRITICAL(&s_feedback_mux);
    if (!active || now_us - last > DP_HAPTIC_HOLD_US) {
        if (tone_hz != NULL) {
            *tone_hz = 0;
        }
        return 0;
    }
    return pad_haptic_pulse_step(sample, (uint32_t)((now_us - start) / 1000),
                                 remain_ms, tone_hz);
}

/**
 * 排空主机输出原始采集：BLE 写侧只入环（NimBLE 主机任务），串口发送集中在
 * 数据面任务（与反馈帧同一约束）。缓冲是本任务专用的静态区，不占任务栈。
 */
static void drain_host_capture(void)
{
    if (!input_link_active() || !dp_capture_enabled()) {
        return;
    }
    static uint8_t payload[INPUT_FRAME_WIRE_MAX_PAYLOAD];
    for (int i = 0; i < DP_CAPTURE_DRAIN_MAX; i++) {
        uint8_t slot = 0;
        const size_t len = dp_capture_pop_payload(payload, sizeof(payload), &slot);
        if (len == 0) {
            break;
        }
        input_link_send_host_raw(slot, payload, len);
    }
}

/** 采样蜂鸣的使能判据：跟输入设备的接入方式走——有线接入（USB host 直插，
 *  或桥接转发的有线手柄）用板载蜂鸣器把采样放成声音；蓝牙手柄按约定丢弃
 *  （采样点播的提示音在蓝牙场景不发也不震）。 */
static bool haptic_buzzer_enabled(void)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    pad_conn_t conn = PAD_CONN_UNKNOWN;
    if (!usb_input_device_ids(&vid, &pid, &conn) &&
        !input_source_device_ids(&vid, &pid, &conn)) {
        return false;
    }
    return conn == PAD_CONN_USB;
}

/** DS4 / DS5 手柄行为的跨采样状态：触摸板按下的键位锁存与触点触发先后的
 *  比较（见 pad/ds_behavior.h）。 */
static pad_ds_state_t s_ds_behavior;

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
    /* 上一拍蜂鸣的响/停状态：只在音色段边界翻转，段起点鸣一次、时长取段
     * 剩余（蜂鸣器非阻塞，重复鸣叫会重置停鸣定时器）。 */
    uint8_t buzzer_env = 0;
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
        /* DS4 / DS5 手柄行为（「DS4、DS5 设置」页两项开关）：触摸板按下的
         *  键位在这一拍定一次（左半减号 / 右半加号 / 截图），下游的目标编码
         *  与屏幕操控看到的都是改写后的键位。 */
        const app_config_t *ds_cfg = app_config_get();
        const pad_ds_config_t ds_config = {
            .touchpad_plus_minus = ds_cfg->ds_touchpad_plus_minus,
            .capture_key = ds_cfg->ds_capture_key,
        };
        pad_ds_apply(&s_ds_behavior, &ds_config, &pad);
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
        /* 采样音色的当前播放位：主机只重发采样 ID（0x0A 采样流），播放节奏
         *  由音色表给出。采样是主机点播的提示音（真手柄用 HD 马达放声），
         *  本设备不把它转成震动：输入设备有线接入时由板载蜂鸣器在「发声」
         *  段按音高发声，蓝牙手柄由 HD 通路把发声段折进音圈——马达编码与
         *  触觉合成只吃 0x30 震动载波。 */
        uint32_t haptic_remain_ms = 0;
        uint16_t haptic_tone_hz = 0;
        const uint8_t haptic_env =
            held_haptic_envelope(esp_timer_get_time(), &haptic_remain_ms,
                                 &haptic_tone_hz);
        if (haptic_env != buzzer_env) {
            buzzer_env = haptic_env;
            /* 蜂鸣器只放「发声」段：强震段是给马达/音圈的震动，小喇叭跟着响
             * 会把节奏搅成连续噪音。鸣叫时长跟段走，音高用音色表给的
             * （定位呼叫的两声上行短鸣）；蜂鸣器超 1 秒的鸣叫会被驱动截断。 */
            if (haptic_buzzer_enabled() && haptic_env == PAD_HAPTIC_BEEP) {
                if (haptic_remain_ms > 1000u) {
                    haptic_remain_ms = 1000u;
                }
                buzzer_beep_tone(haptic_tone_hz, haptic_remain_ms);
            }
        }
        /* 采样音色的段边界按 tick 投递：段是固件合成的（主机只给采样 ID 与
         *  起停），而 FEEDBACK 帧只在主机事件到达时才发——查找手柄页的采样
         *  事件约 15Hz，段边界会被量化到 64ms 的栅格（震动/蜂鸣的起止错位、
         *  短段整段丢失；板载蜂鸣器按 tick 走，两条通路因此还不同步）。
         *  段状态（幅度段 + 段音高）一变就置待发位，投递精度回到 tick。 */
        if (feedback_sent_valid &&
            pad_feedback_segment_changed(&feedback_sent, haptic_env, haptic_tone_hz)) {
            portENTER_CRITICAL(&s_feedback_mux);
            s_feedback_pending = true;
            portEXIT_CRITICAL(&s_feedback_mux);
        }
        if (s_feedback_pending) {
            pad_feedback_t feedback;
            portENTER_CRITICAL(&s_feedback_mux);
            feedback = s_feedback;
            s_feedback_pending = false;
            portEXIT_CRITICAL(&s_feedback_mux);
            /* 采样超时自灭：只在本地副本上清（FEEDBACK 帧的采样字节归零），
             *  不动持续帧——新采样事件会刷新时戳并重新触发。 */
            if (feedback.haptic_sample_valid &&
                esp_timer_get_time() - s_haptic_last_us > DP_HAPTIC_HOLD_US) {
                feedback.haptic_sample_valid = false;
                feedback.haptic_sample = 0;
            }
            /* 写回（USB OUT / 桥接 OUT_REPORT）按「编码后的报告字节变了才发」：
             *  主机的震动流是音频式连续包络，原始参数包逐包都在抖，但真正落到
             *  马达/灯字节的值常常几十包不变——只有字节变化的帧才值得占一次
             *  传输，PC 会话循环才腾得出手转发输入。同代透传（NS2 手柄）的
             *  参数包原样在编码字节里，逐包纹理照常透传。 */
            uint8_t feedback_out[PAD_OUTPUT_MAX];
            size_t feedback_out_len =
                encode_feedback_report(&feedback, feedback_out, sizeof(feedback_out));
            const pad_layout_t *feedback_layout =
                feedback_out_len > 0 ? pad_feedback_last_layout() : NULL;

            /* 采样音色的当前段随持续帧走：强震段铺音圈、发声段铺扬声器
             *  （音高随段带下）。先落段再做 HD 渲染，子帧表才是这一拍的
             *  铺色结果。 */
            feedback.haptic_env = haptic_env;
            feedback.haptic_tone_hz = haptic_env == PAD_HAPTIC_BEEP ? haptic_tone_hz : 0;

            /* HD 触觉映射（映射在布局内完成）：布局行声明了 HD 通路才把主机
             * 波形重整成本设备的时序子帧组——USB 直插走板上合成，桥接经 FEEDBACK
             * 帧交 PC 哑渲染（音频触觉让位与采样发声段都从这一份走）。 */
            pad_hd_render_t hd_render;
            pad_feedback_hd_render(feedback_layout, &feedback, &hd_render);
            const bool hd_active =
                feedback_layout != NULL && feedback_layout->out.hd.ops != 0;

            /* 音频触觉让位（USB 直插走板上合成、桥接走 PC 侧合成）：两颗音圈
             *  被 HID 与音频同时驱动会叠成浑浊触感，接手的一条通路拿到的 HID
             *  报告把震动字段清零。子帧（含频率落地）随持续帧走，两侧合成
             *  同一份数值（PC 只做哑渲染，落地规则只在固件里有一份）。 */
            const bool usb_audio_engaged = usb_input_attached() && feedback_layout != NULL &&
                                           feedback_layout->out.audio_haptic &&
                                           usb_input_audio_haptics();
            const bool bridge_audio_engaged = input_source_attached() && feedback_layout != NULL &&
                                              feedback_layout->out.audio_haptic &&
                                              s_bridge_audio_haptics;
            /* 没有音频承载时的兜底：强震段折进马达本地写回（蓝牙没开 0x32 流、
             *  音频端点开不起来），查找手柄至少摸得到。折进只作用于写回帧，
             *  FEEDBACK 状态帧仍带主机的真实波形。 */
            const pad_feedback_t *writeback_fb = &feedback;
            pad_feedback_t folded;
            if (haptic_env == PAD_HAPTIC_PULSE && !usb_audio_engaged &&
                !bridge_audio_engaged) {
                folded = feedback;
                pad_feedback_fold_pulse_motors(&folded, PAD_HAPTIC_PULSE);
                writeback_fb = &folded;
                feedback_out_len =
                    encode_feedback_report(writeback_fb, feedback_out, sizeof(feedback_out));
            }
            if (usb_audio_engaged) {
                haptic_synth_params_t haptic;
                haptic.amp_peak = feedback_layout->out.hd.amp_peak;
                /* 每个子帧的帧数 = rate × cycle_ms / 1000 / 3（整数帧，引擎
                 * 按它倒数切帧）。 */
                haptic.slice_frames = (uint16_t)(
                    (uint32_t)feedback_layout->out.hd.rate_hz *
                    feedback_layout->out.hd.cycle_ms / 3000u);
                haptic.tones = hd_render;
                usb_input_haptic(&haptic);
            }

            /* USB 直插写回：让位时发清了震动字节的报告（玩家灯照常）。 */
            if (usb_input_attached() && feedback_out_len > 0) {
                uint8_t usb_out[PAD_OUTPUT_MAX];
                size_t usb_out_len = feedback_out_len;
                memcpy(usb_out, feedback_out, feedback_out_len);
                if (usb_audio_engaged) {
                    usb_out_len = encode_quiet_feedback(&feedback, usb_out, sizeof(usb_out));
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
                    bridge_out_len = encode_quiet_feedback(&feedback, bridge_out,
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
             *  原始字节的抖动不产生新帧；采样字节带原始采样 ID，只供 PC 日志
             *  展示。布局行声明 HD 通路时载荷扩到 57 字节，附上重整后的时序子帧
             *  组（PC 侧音频触觉与蓝牙私有流按它哑渲染）。 */
            if (!feedback_sent_valid || !pad_feedback_equal(&feedback_sent, &feedback)) {
                uint8_t feedback_payload[PAD_FEEDBACK_WIRE_HD];
                const size_t feedback_payload_len =
                    pad_feedback_wire(&feedback, hd_active ? &hd_render : NULL,
                                      feedback_payload, sizeof(feedback_payload));
                if (feedback_payload_len > 0) {
                    input_link_send_feedback(feedback_payload, feedback_payload_len);
                }
                feedback_sent = feedback;
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
        drain_host_capture();
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

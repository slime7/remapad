#include "ns2_output.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ns2_report.h"

static const char *TAG = "remapad_ns2out";

/** NTAG215 用户区完整镜像（ amiibo dump 通行尺寸：135 页 × 4B = 540B）。 */
#define NS2_AMIIBO_MAX 540

/** 会话当前报告格式（0x05/0x09）。ns2 层不反向包含 ble 头，链接期解析。 */
extern uint8_t ns2_session_report_format(void);

static struct {
    ns2_output_sink_t sink;
    ns2_feedback_fn feedback_fn;
    void *feedback_user;

    uint8_t counter09;
    uint32_t counter05;

    uint8_t battery_level;
    uint16_t battery_mv;
    bool charging;
    bool external_power;

    uint8_t *amiibo;
    size_t amiibo_len;
} s_out;

void ns2_output_set_sink(const ns2_output_sink_t *sink)
{
    if (sink != NULL) {
        s_out.sink = *sink;
    } else {
        memset(&s_out.sink, 0, sizeof(s_out.sink));
    }
}

void ns2_output_set_feedback_listener(ns2_feedback_fn fn, void *user)
{
    s_out.feedback_fn = fn;
    s_out.feedback_user = user;
}

void ns2_output_send(const ns2_controller_state_t *state)
{
    if (s_out.sink.send_report == NULL) {
        return;
    }
    /* 电池字段以 ns2_output_set_battery 的最新值为准（输入源可能不带电池）。 */
    ns2_controller_state_t merged = *state;
    if (s_out.battery_level != 0 || s_out.battery_mv != 0) {
        merged.battery_level = s_out.battery_level;
        merged.battery_mv = s_out.battery_mv;
        merged.charging = s_out.charging;
        merged.external_power = s_out.external_power;
    }
    const uint8_t format = ns2_session_report_format();
    if (s_out.sink.ready != NULL && !s_out.sink.ready(format, s_out.sink.user)) {
        return;
    }
    if (format == NS2_REPORT_ID_05) {
        uint8_t report[NS2_INPUT_05_LEN];
        ns2_encode_input_05(report, &merged, s_out.counter05++);
        s_out.sink.send_report(format, report, sizeof(report), s_out.sink.user);
    } else {
        uint8_t report[NS2_INPUT_09_LEN];
        ns2_encode_input_09(report, &merged, s_out.counter09++);
        s_out.sink.send_report(format, report, sizeof(report), s_out.sink.user);
    }
}

void ns2_output_set_battery(uint8_t level, uint16_t voltage_mv, bool charging, bool external)
{
    s_out.battery_level = level;
    s_out.battery_mv = voltage_mv;
    s_out.charging = charging;
    s_out.external_power = external;
}

esp_err_t ns2_output_amiibo_stage(const uint8_t *data, size_t len)
{
    if (len == 0 || data == NULL) {
        if (s_out.amiibo != NULL) {
            heap_caps_free(s_out.amiibo);
            s_out.amiibo = NULL;
            s_out.amiibo_len = 0;
        }
        return ESP_OK;
    }
    if (len > NS2_AMIIBO_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_out.amiibo == NULL) {
        s_out.amiibo = heap_caps_malloc(NS2_AMIIBO_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_out.amiibo == NULL) {
            s_out.amiibo = heap_caps_malloc(NS2_AMIIBO_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_out.amiibo == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(s_out.amiibo, data, len);
    s_out.amiibo_len = len;
    ESP_LOGI(TAG, "amiibo staged: %u bytes (nfc state -> ready)", (unsigned)len);
    return ESP_OK;
}

bool ns2_output_amiibo_ready(void)
{
    return s_out.amiibo != NULL;
}

size_t ns2_output_amiibo_read(uint32_t offset, uint8_t *out, size_t len)
{
    if (out == NULL || len == 0 || s_out.amiibo == NULL || offset >= s_out.amiibo_len) {
        return 0;
    }
    const size_t avail = s_out.amiibo_len - offset;
    const size_t n = len < avail ? len : avail;
    memcpy(out, &s_out.amiibo[offset], n);
    return n;
}

uint8_t ns2_output_nfc_state(void)
{
    /* 预置就绪汇报 0x01（检测到标签入场）；完整感应流程状态（0x02-0x07）
     * 待 Command 0x01 NFC 通路实现后由会话层驱动。 */
    return s_out.amiibo != NULL ? 0x01u : 0x00u;
}

void ns2_output_emit_rumble(const ns2_rumble_event_t *event)
{
    if (s_out.feedback_fn != NULL && event != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_RUMBLE, event, s_out.feedback_user);
    }
}

void ns2_output_emit_player_led(uint8_t led_mask)
{
    if (s_out.feedback_fn != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_PLAYER_LED, &led_mask, s_out.feedback_user);
    }
}

void ns2_output_emit_haptic_sample(uint8_t sample_id)
{
    if (s_out.feedback_fn != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_HAPTIC_SAMPLE, &sample_id, s_out.feedback_user);
    }
}

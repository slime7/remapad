#include "ns2_output.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ns2_report.h"

static const char *TAG = "remapad_ns2out";

/** NTAG215 用户区完整镜像（ amiibo dump 通行尺寸：135 页 × 4B = 540B）。 */
#define NS2_AMIIBO_MAX 540

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

    uint8_t motion_mode; /* ns2_motion_mode_t：0x09 运动块占位方式 */
    bool rumble_enabled; /* 主机开启了触觉特性 */

    /* 耳机状态字节（0x09 偏移 0x0D，同时决定 0x05 的插入位）：派生值来自
     * 输入设备的 3.5mm 状态，覆盖值供实机 A/B（串口 headset 命令）。 */
    bool headset_override_on;
    uint8_t headset_override;
    uint8_t headset_derived;

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
    if (s_out.sink.session_count == NULL || s_out.sink.session_info == NULL ||
        s_out.sink.send_report == NULL) {
        return;
    }
    /* 电池字段以 ns2_output_set_battery 的最新值为准（输入源可能不带电池）。 */
    ns2_controller_state_t merged = *state;
    merged.motion_mode = s_out.motion_mode;
    merged.headset_state = ns2_output_headset_byte();
    if (s_out.battery_level != 0 || s_out.battery_mv != 0) {
        merged.battery_level = s_out.battery_level;
        merged.battery_mv = s_out.battery_mv;
        merged.charging = s_out.charging;
        merged.external_power = s_out.external_power;
    }
    const size_t sessions = s_out.sink.session_count(s_out.sink.user);
    for (size_t i = 0; i < sessions; i++) {
        uint8_t identity = NS2_ID_PRO;
        uint8_t format = NS2_REPORT_ID_09;
        if (!s_out.sink.session_info(i, &identity, &format, s_out.sink.user)) {
            continue;
        }
        /* 通用报告（0x05，USB 模式）与专用报告（0x09，蓝牙）两种报文体，
         * 长度都是 63 字节，按会话格式选编码器。 */
        uint8_t report[NS2_INPUT_09_LEN];
        if (format == NS2_REPORT_ID_05) {
            ns2_encode_input_05(report, &merged, s_out.counter05++);
        } else {
            ns2_encode_input_09(report, &merged, s_out.counter09++);
        }
        s_out.sink.send_report(i, format, report, sizeof(report), s_out.sink.user);
    }
}

void ns2_output_set_battery(uint8_t level, uint16_t voltage_mv, bool charging, bool external)
{
    s_out.battery_level = level;
    s_out.battery_mv = voltage_mv;
    s_out.charging = charging;
    s_out.external_power = external;
}

void ns2_output_set_motion_mode(uint8_t mode)
{
    s_out.motion_mode = mode;
}

void ns2_output_set_rumble_enabled(bool enabled)
{
    s_out.rumble_enabled = enabled;
}

uint8_t ns2_output_motion_mode(void)
{
    return s_out.motion_mode;
}

void ns2_output_set_headset_derived(uint8_t value)
{
    s_out.headset_derived = value;
}

uint8_t ns2_output_headset_derived(void)
{
    return s_out.headset_derived;
}

void ns2_output_set_headset_override(bool enabled, uint8_t value)
{
    s_out.headset_override_on = enabled;
    s_out.headset_override = value;
}

bool ns2_output_headset_override(uint8_t *out_value)
{
    if (out_value != NULL) {
        *out_value = s_out.headset_override;
    }
    return s_out.headset_override_on;
}

uint8_t ns2_output_headset_byte(void)
{
    return s_out.headset_override_on ? s_out.headset_override : s_out.headset_derived;
}

bool ns2_output_send_raw(const pad_state_t *pad)
{
    if (pad == NULL || pad->raw_len == 0 || s_out.sink.session_count == NULL ||
        s_out.sink.session_info == NULL || s_out.sink.send_report == NULL) {
        return false;
    }
    const uint8_t report_id = pad->raw_report_id;
    if (report_id != NS2_REPORT_ID_05 && report_id != NS2_REPORT_ID_09) {
        return false;
    }
    const size_t body_len = report_id == NS2_REPORT_ID_09 ? NS2_INPUT_09_LEN : NS2_INPUT_05_LEN;
    if (pad->raw_len != body_len + 1u) {
        return false;
    }
    uint8_t body[NS2_INPUT_09_LEN];
    size_t delivered = 0;
    const size_t sessions = s_out.sink.session_count(s_out.sink.user);
    for (size_t i = 0; i < sessions; i++) {
        uint8_t identity = NS2_ID_PRO;
        uint8_t format = NS2_REPORT_ID_09;
        if (!s_out.sink.session_info(i, &identity, &format, s_out.sink.user)) {
            continue;
        }
        /* 会话身份只有 Pro 一种，透传只按报告格式对账：同代手柄的报文体与目标
         * 语言一致时原样转发（ADR 0026），格式对不上就回落解析重编码。 */
        if (format != report_id) {
            continue;
        }
        memcpy(body, &pad->raw[1], body_len);
        if (report_id == NS2_REPORT_ID_09) {
            body[NS2_09_OFF_STATUS] = s_out.rumble_enabled ? 0x38 : 0x30;
            body[NS2_09_OFF_NFC] = ns2_output_nfc_state();
            /* 耳机状态与编码路径同源：PC 手柄的 3.5mm 状态或串口覆盖值。 */
            body[NS2_09_OFF_HEADSET] = ns2_output_headset_byte();
        }
        s_out.sink.send_report(i, report_id, body, body_len, s_out.sink.user);
        delivered++;
    }
    return delivered > 0;
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

/** 「在震」的载波电平上限（8 位刻度）：主机在「查找手柄」页会用 1-2/255 的
 *  高频振幅维持 LRA 通路（蜂鸣本体由 0x0A 采样流承载），这种电平在任何
 *  马达上都感知不到。归一强度不超过它的参数包不算在震。 */
#define NS2_RUMBLE_CARRIER_MAX 2u

void ns2_rumble_band_strengths(const uint8_t raw[16], uint8_t *lf, uint8_t *hf)
{
    if (lf != NULL) {
        *lf = 0;
    }
    if (hf != NULL) {
        *hf = 0;
    }
    if (raw == NULL) {
        return;
    }
    /* 参数包：字节 0 是状态字，其后三组各 5 字节（低频频率 9 位 + 低频振幅
     * 10 位 + 高频频率 9 位 + 高频振幅 8 位，小端位序）。逐带取三组最大振幅，
     * 低频 10 位右移两位压到 8 位刻度，与私有的 0-255 强度对齐。 */
    uint8_t best_lf = 0;
    uint8_t best_hf = 0;
    for (size_t g = 0; g < 3; g++) {
        const uint8_t *p = &raw[1 + g * 5];
        uint64_t v = 0;
        for (size_t i = 0; i < 5; i++) {
            v |= (uint64_t)p[i] << (8 * i);
        }
        const uint8_t group_lf = (uint8_t)(((v >> 9) & 0x3FFu) >> 2);
        const uint8_t group_hf = (uint8_t)((v >> 28) & 0xFFu);
        if (group_lf > best_lf) {
            best_lf = group_lf;
        }
        if (group_hf > best_hf) {
            best_hf = group_hf;
        }
    }
    if (lf != NULL) {
        *lf = best_lf;
    }
    if (hf != NULL) {
        *hf = best_hf;
    }
}

uint8_t ns2_rumble_strength(const uint8_t raw[16])
{
    uint8_t lf = 0;
    uint8_t hf = 0;
    ns2_rumble_band_strengths(raw, &lf, &hf);
    return hf > lf ? hf : lf;
}

void ns2_rumble_band_frequencies(const uint8_t raw[16], uint16_t *lf_hz, uint16_t *hf_hz)
{
    if (lf_hz != NULL) {
        *lf_hz = 0;
    }
    if (hf_hz != NULL) {
        *hf_hz = 0;
    }
    if (raw == NULL) {
        return;
    }
    /* 频率字段与振幅同处一组 5 字节小端位串：低频在位 0-8、高频在位 19-27，
     * 同样逐组取最大。 */
    uint16_t best_lf = 0;
    uint16_t best_hf = 0;
    for (size_t g = 0; g < 3; g++) {
        const uint8_t *p = &raw[1 + g * 5];
        uint64_t v = 0;
        for (size_t i = 0; i < 5; i++) {
            v |= (uint64_t)p[i] << (8 * i);
        }
        const uint16_t group_lf = (uint16_t)(v & 0x1FFu);
        const uint16_t group_hf = (uint16_t)((v >> 19) & 0x1FFu);
        if (group_lf > best_lf) {
            best_lf = group_lf;
        }
        if (group_hf > best_hf) {
            best_hf = group_hf;
        }
    }
    if (lf_hz != NULL) {
        *lf_hz = best_lf;
    }
    if (hf_hz != NULL) {
        *hf_hz = best_hf;
    }
}

bool ns2_rumble_parse(const uint8_t *data, size_t len, ns2_rumble_event_t *out)
{
    if (data == NULL || out == NULL) {
        return false;
    }
    /* 32 字节 = BLE 形态（实机写入即是此长度，不带 Report ID）；
     * 33 字节及以上 = 多带 1 字节 Report ID/占位前缀的形态。 */
    const size_t body = len >= 1 + 32 ? 1 : 0;
    if (len < body + 32) {
        return false;
    }
    memcpy(out->raw, &data[body], sizeof(out->raw));
    /* LRA 状态字 bit6 = 启用标志（controller.md「输出报告格式」）。游戏里主机以接近输入
     * 上报的频率持续刷「保活包」：使能位为 1、三组振幅全 0，真机收到同样毫无
     * 动静。「在震」必须是使能且归一强度非零——把使能位直接当在震，输入手柄
     * 会被写上一场主机根本没有的震动。非零的门槛还要高过载波电平：「查找
     * 手柄」页的参数包带着 1-2/255 的高频振幅维持 LRA 通路（蜂鸣本体走 0x0A
     * 采样流），这种电平任何马达都感知不到，判成在震会把采样退化出的短震动
     * 压掉（2026-09-18 实机：高频 1/1、点击手柄毫无动静）。 */
    out->left_on = (out->raw[0] & 0x40) != 0 &&
                   ns2_rumble_strength(out->raw) > NS2_RUMBLE_CARRIER_MAX;
    out->right_on = (out->raw[16] & 0x40) != 0 &&
                    ns2_rumble_strength(&out->raw[16]) > NS2_RUMBLE_CARRIER_MAX;
    return true;
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

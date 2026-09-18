#include "usb_input.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "dp_source.h"
#include "pad_device.h"
#include "usb_audio.h"
#include "usb_transport.h"

static const char *TAG = "remapad_usb_src";

static struct {
    portMUX_TYPE mux;
    bool attached;
    uint16_t vid;
    uint16_t pid;
    pad_report_t report;
    uint32_t reports;
    uint32_t outputs;
    char desc[48];
} s_usb = {.mux = portMUX_INITIALIZER_UNLOCKED, .desc = "none"};

static const char *conn_name(pad_conn_t conn)
{
    switch (conn) {
    case PAD_CONN_USB:
        return "usb";
    case PAD_CONN_BT:
        return "bt";
    default:
        return "-";
    }
}

void usb_input_note_device(bool attached, uint16_t vid, uint16_t pid)
{
    portENTER_CRITICAL(&s_usb.mux);
    s_usb.attached = attached;
    s_usb.vid = vid;
    s_usb.pid = pid;
    memset(&s_usb.report, 0, sizeof(s_usb.report));
    if (attached) {
        /* 家族留给 pad/ 判定：这里只带设备标识，与桥接路径的语义一致。 */
        s_usb.report.family = PAD_FAMILY_UNKNOWN;
        s_usb.report.conn = PAD_CONN_USB;
        s_usb.report.vid = vid;
        s_usb.report.pid = pid;
        snprintf(s_usb.desc, sizeof(s_usb.desc), "usb %04x:%04x", vid, pid);
    } else {
        snprintf(s_usb.desc, sizeof(s_usb.desc), "none");
    }
    portEXIT_CRITICAL(&s_usb.mux);
    if (attached) {
        ESP_LOGI(TAG, "pad attached: %s family=%s conn=%s", usb_input_device_desc(),
                 pad_family_name(pad_family_from_ids(vid, pid)), conn_name(PAD_CONN_USB));
    } else {
        ESP_LOGI(TAG, "pad detached, input returns to neutral");
    }
}

void usb_input_submit_report(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > PAD_REPORT_MAX) {
        return;
    }
    portENTER_CRITICAL(&s_usb.mux);
    if (!s_usb.attached) {
        /* 没收到接入事件就来了报告（部分设备不上报枚举完成）：按已接入处理。 */
        s_usb.attached = true;
        s_usb.report.family = PAD_FAMILY_UNKNOWN;
        s_usb.report.conn = PAD_CONN_USB;
        s_usb.report.vid = s_usb.vid;
        s_usb.report.pid = s_usb.pid;
    }
    s_usb.report.report_id = data[0];
    s_usb.report.len = (uint8_t)len;
    memcpy(s_usb.report.data, data, len);
    s_usb.report.seq++;
    s_usb.reports++;
    portEXIT_CRITICAL(&s_usb.mux);
}

/** 采样：没有设备接入时保持静置（pad_state_defaults 的值）。 */
static void usb_sample(pad_state_t *state)
{
    pad_report_t report;
    bool attached = false;
    portENTER_CRITICAL(&s_usb.mux);
    report = s_usb.report;
    attached = s_usb.attached;
    portEXIT_CRITICAL(&s_usb.mux);
    if (!attached || report.len == 0) {
        return;
    }
    pad_state_from_report(&report, state);
}

static const dp_source_t s_usb_source = {
    .name = "usb",
    .sample = usb_sample,
};

void usb_input_register(void)
{
    dp_source_register(&s_usb_source);
}

bool usb_input_attached(void)
{
    return s_usb.attached;
}

bool usb_input_device_ids(uint16_t *vid, uint16_t *pid, pad_conn_t *conn)
{
    if (!s_usb.attached) {
        return false;
    }
    if (vid != NULL) {
        *vid = s_usb.vid;
    }
    if (pid != NULL) {
        *pid = s_usb.pid;
    }
    if (conn != NULL) {
        *conn = PAD_CONN_USB;
    }
    return true;
}

const char *usb_input_device_desc(void)
{
    return s_usb.desc;
}

uint32_t usb_input_report_count(void)
{
    return s_usb.reports;
}

uint32_t usb_input_output_count(void)
{
    return s_usb.outputs;
}

void usb_input_send_output(const uint8_t *report, size_t len)
{
    if (!usb_input_attached() || report == NULL || len == 0) {
        return;
    }
    usb_host_queue_output(report, len);
    s_usb.outputs++;
}

bool usb_input_audio_haptics(void)
{
    return usb_audio_streaming();
}

void usb_input_haptic(const haptic_synth_params_t *params)
{
    usb_audio_haptic(params);
}

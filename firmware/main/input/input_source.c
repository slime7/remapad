#include "input_source.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "dp_source.h"
#include "pad_device.h"

static const char *TAG = "remapad_input_src";

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

static struct {
    portMUX_TYPE mux;
    bool attached;
    pad_report_t report;
    uint32_t reports;
    char desc[48];
} s_in = { .mux = portMUX_INITIALIZER_UNLOCKED, .desc = "none" };

/** 载荷前 8 字节是设备标识：家族、连接、VID/PID 小端、Report ID、报告长度。 */
static void parse_device_id(const uint8_t *payload, size_t len, pad_report_t *report)
{
    if (len < INPUT_DEVICE_ID_LEN) {
        return;
    }
    report->family = (pad_family_t)payload[0];
    report->conn = (pad_conn_t)payload[1];
    report->vid = (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8));
    report->pid = (uint16_t)(payload[4] | ((uint16_t)payload[5] << 8));
    report->report_id = payload[6];
    report->len = payload[7];
}

static void update_desc(const pad_report_t *report, bool attached)
{
    if (!attached) {
        snprintf(s_in.desc, sizeof(s_in.desc), "none");
        return;
    }
    snprintf(s_in.desc, sizeof(s_in.desc), "%s %s %04x:%04x len=%u",
             pad_family_name(report->family), conn_name(report->conn), report->vid,
             report->pid, (unsigned)report->report_id);
}

static void handle_attach(const input_frame_view_t *frame)
{
    pad_report_t report;
    memset(&report, 0, sizeof(report));
    parse_device_id(frame->payload, frame->payload_len, &report);
    portENTER_CRITICAL(&s_in.mux);
    s_in.report = report;
    s_in.report.len = 0;
    s_in.attached = true;
    update_desc(&report, true);
    portEXIT_CRITICAL(&s_in.mux);
    ESP_LOGI(TAG, "pad attached: %s", input_source_device_desc());
}

static void handle_detach(void)
{
    portENTER_CRITICAL(&s_in.mux);
    s_in.attached = false;
    s_in.report.len = 0;
    update_desc(&s_in.report, false);
    portEXIT_CRITICAL(&s_in.mux);
    ESP_LOGI(TAG, "pad detached, input returns to neutral");
}

static void handle_report(const input_frame_view_t *frame)
{
    if (frame->payload_len <= INPUT_DEVICE_ID_LEN) {
        return;
    }
    const size_t body_len = frame->payload_len - INPUT_DEVICE_ID_LEN;
    const size_t copy_len = body_len < PAD_REPORT_MAX ? body_len : PAD_REPORT_MAX;
    portENTER_CRITICAL(&s_in.mux);
    parse_device_id(frame->payload, frame->payload_len, &s_in.report);
    s_in.report.len = (uint8_t)copy_len;
    s_in.report.seq = frame->seq;
    memcpy(s_in.report.data, &frame->payload[INPUT_DEVICE_ID_LEN], copy_len);
    s_in.reports++;
    if (!s_in.attached) {
        /* PC 侧没有先发接入帧也能工作：第一份报告即视为接入。 */
        s_in.attached = true;
        update_desc(&s_in.report, true);
    }
    portEXIT_CRITICAL(&s_in.mux);
}

void input_source_handle_frame(const input_frame_view_t *frame)
{
    switch (frame->type) {
    case INPUT_FRAME_TYPE_ATTACH:
        handle_attach(frame);
        break;
    case INPUT_FRAME_TYPE_DETACH:
        handle_detach();
        break;
    case INPUT_FRAME_TYPE_REPORT:
        handle_report(frame);
        break;
    default:
        ESP_LOGW(TAG, "unsupported frame type 0x%02x", frame->type);
        break;
    }
}

void input_source_note_link_down(void)
{
    handle_detach();
}

static void bridge_sample(pad_state_t *state)
{
    pad_report_t report;
    bool attached = false;
    portENTER_CRITICAL(&s_in.mux);
    report = s_in.report;
    attached = s_in.attached;
    portEXIT_CRITICAL(&s_in.mux);
    if (!attached || report.len == 0) {
        /* 未接入：保持 pad_state_defaults 的静置值。 */
        return;
    }
    pad_state_from_report(&report, state);
}

static const dp_source_t s_bridge_source = {
    .name = "bridge",
    .sample = bridge_sample,
};

void input_source_register(void)
{
    dp_source_register(&s_bridge_source);
}

bool input_source_attached(void)
{
    return s_in.attached;
}

uint32_t input_source_report_count(void)
{
    return s_in.reports;
}

bool input_source_device_ids(uint16_t *vid, uint16_t *pid, pad_conn_t *conn)
{
    if (!s_in.attached) {
        return false;
    }
    if (vid != NULL) {
        *vid = s_in.report.vid;
    }
    if (pid != NULL) {
        *pid = s_in.report.pid;
    }
    if (conn != NULL) {
        *conn = s_in.report.conn;
    }
    return true;
}

const char *input_source_device_desc(void)
{
    return s_in.desc;
}

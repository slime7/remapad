#include "input_link.h"

#include <stdbool.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amiibo_session.h"
#include "cli.h"
#include "input_frame.h"
#include "input_source.h"
#include "ota_session.h"

static const char *TAG = "remapad_input";

/** RX 环形缓冲要能吃下一整帧加一段命令行：太小会在日志刷屏时丢字节。 */
#define INPUT_LINK_RX_BUF 4096
#define INPUT_LINK_TX_BUF 1024
/* CLI 桥接命令在这条任务里执行：amiibo 槽位读写走 SPIFFS/VFS 的 fopen
 * 调用链（栈深），4096 会溢出（实机 2026-09-19 select 即溢出），定 8192。 */
#define INPUT_LINK_TASK_STACK 8192
#define INPUT_LINK_TASK_PRIO 6
/** 控制帧（PING/OTA 应答）等着写进发送环的上限：日志刷屏时环会满，但绝不无限等。 */
#define INPUT_LINK_REPLY_TIMEOUT_MS 200u
/** 单次尝试的等待粒度：够让驱动冲掉一批日志字节，又不至于卡住调用任务。 */
#define INPUT_LINK_TX_SLICE_MS 20u

static input_frame_rx_t s_rx;
static uint32_t s_frames;
static volatile bool s_running;
static TaskHandle_t s_task;

static void on_frame(const input_frame_view_t *frame, void *user)
{
    (void)user;
    if (frame->version != INPUT_FRAME_VERSION) {
        ESP_LOGW(TAG, "frame version %u ignored (expected %u)", frame->version,
                 INPUT_FRAME_VERSION);
        return;
    }
    s_frames++;
    /* 升级帧由 OTA 会话接走（要写 flash，不能落在输入通路里）；amiibo 上传
     * 帧由 amiibo 会话接走（要写 NVS）；探测帧在这里直接应答，其余交给输入源。 */
    if (ota_session_is_frame_type(frame->type)) {
        ota_session_handle_frame(frame);
        return;
    }
    if (amiibo_session_is_frame_type(frame->type)) {
        amiibo_session_handle_frame(frame);
        return;
    }
    if (frame->type == INPUT_FRAME_TYPE_PING) {
        const uint8_t version = INPUT_FRAME_VERSION;
        input_link_send_frame_wait(INPUT_FRAME_TYPE_PING, 0, &version, sizeof(version),
                                   INPUT_LINK_REPLY_TIMEOUT_MS);
        ESP_LOGI(TAG, "bridge ping from PC (protocol v%u)",
                 frame->payload_len > 0 ? frame->payload[0] : 0u);
        return;
    }
    input_source_handle_frame(frame);
}

static void on_text(const uint8_t *text, size_t len, void *user)
{
    (void)user;
    cli_feed_bytes(text, len);
}

static void input_link_task(void *param)
{
    (void)param;
    uint8_t buf[256];
    ESP_LOGI(TAG, "bridge link ready on USB-Serial/JTAG");
    while (s_running) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            input_frame_rx_feed(&s_rx, buf, (size_t)n, on_frame, on_text, NULL);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t input_link_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = INPUT_LINK_TX_BUF,
        .rx_buffer_size = INPUT_LINK_RX_BUF,
    };
    const esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        return err;
    }
    /* 驱动就绪后把 vfs 切到驱动路径：日志与 CLI 回复走驱动的发送环形缓冲，
     * 与这里的接收共用同一个驱动，不再直读硬件 FIFO。 */
    usb_serial_jtag_vfs_use_driver();
    input_frame_rx_reset(&s_rx);
    s_running = true;
    if (xTaskCreate(input_link_task, "remapad-input", INPUT_LINK_TASK_STACK, NULL,
                    INPUT_LINK_TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void input_link_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    for (int i = 0; i < 20 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    /* 之后 PC 再也发不进报告：清掉接入状态，避免旧设备标识被拿去查反馈表。 */
    input_source_note_link_down();
    usb_serial_jtag_driver_uninstall();
    ESP_LOGI(TAG, "bridge link released usb-serial/jtag");
}

bool input_link_active(void)
{
    return s_running;
}

uint32_t input_link_frame_count(void)
{
    return s_frames;
}

/** 组一帧到调用者的缓冲：载荷超限时返回 0，调用者据此丢弃。 */
static size_t encode_frame(uint8_t *frame, uint8_t type, uint8_t slot, const uint8_t *payload,
                           size_t payload_len)
{
    return input_frame_encode(frame, INPUT_FRAME_MAX_LEN, type, slot, 0, payload, payload_len);
}

/** 把已编码的一帧分片推进发送环：超时即放弃，不无限阻塞调用任务。 */
static esp_err_t send_encoded_wait(const uint8_t *frame, size_t len, uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t sent = 0;
    while (sent < len) {
        const int n = usb_serial_jtag_write_bytes(&frame[sent], len - sent,
                                                  pdMS_TO_TICKS(INPUT_LINK_TX_SLICE_MS));
        if (n > 0) {
            sent += (size_t)n;
        }
        if (sent < len && esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "frame 0x%02x blocked (%u/%u bytes in %u ms)", frame[3], (unsigned)sent,
                     (unsigned)len, (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
    }
    /* 环里还排着日志字节：等驱动推完再返回，调用方紧接着重启也不会截断应答。 */
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(INPUT_LINK_TX_SLICE_MS));
    return ESP_OK;
}

void input_link_send_frame(uint8_t type, uint8_t slot, const uint8_t *payload,
                           size_t payload_len)
{
    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = encode_frame(frame, type, slot, payload, payload_len);
    if (len == 0) {
        return;
    }
    /* 主机没在读时直接丢弃，绝不在数据面任务里阻塞。 */
    usb_serial_jtag_write_bytes(frame, len, 0);
}

esp_err_t input_link_send_frame_wait(uint8_t type, uint8_t slot, const uint8_t *payload,
                                    size_t payload_len, uint32_t timeout_ms)
{
    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = encode_frame(frame, type, slot, payload, payload_len);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_encoded_wait(frame, len, timeout_ms);
}

esp_err_t input_link_send_image_info(uint16_t width, uint16_t height, uint32_t timeout_ms)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[INPUT_FRAME_IMAGE_INFO_LEN] = {
        (uint8_t)(width & 0xFFu),
        (uint8_t)(width >> 8),
        (uint8_t)(height & 0xFFu),
        (uint8_t)(height >> 8),
        INPUT_FRAME_IMAGE_FORMAT_RGB565_LE,
    };
    uint8_t frame[INPUT_FRAME_WIRE_MAX_LEN];
    const size_t len = input_frame_encode_wire(frame, sizeof(frame), INPUT_FRAME_TYPE_IMAGE_INFO, 0,
                                              0, payload, sizeof(payload));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_encoded_wait(frame, len, timeout_ms);
}

esp_err_t input_link_send_image_data(uint32_t offset, const uint8_t *data, size_t len,
                                    uint32_t timeout_ms)
{
    if (!s_running || data == NULL || len == 0 || len > INPUT_FRAME_IMAGE_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t payload[INPUT_FRAME_IMAGE_OFF_LEN + INPUT_FRAME_IMAGE_CHUNK_MAX];
    payload[0] = (uint8_t)(offset & 0xFFu);
    payload[1] = (uint8_t)((offset >> 8) & 0xFFu);
    payload[2] = (uint8_t)((offset >> 16) & 0xFFu);
    payload[3] = (uint8_t)((offset >> 24) & 0xFFu);
    memcpy(&payload[INPUT_FRAME_IMAGE_OFF_LEN], data, len);
    uint8_t frame[INPUT_FRAME_WIRE_MAX_LEN];
    const size_t payload_len = INPUT_FRAME_IMAGE_OFF_LEN + len;
    const size_t frame_len = input_frame_encode_wire(frame, sizeof(frame),
                                                    INPUT_FRAME_TYPE_IMAGE_DATA, 0, 0,
                                                    payload, payload_len);
    if (frame_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_encoded_wait(frame, frame_len, timeout_ms);
}

esp_err_t input_link_send_image_end(uint32_t total_bytes, uint32_t timeout_ms)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[INPUT_FRAME_IMAGE_END_LEN] = {
        (uint8_t)(total_bytes & 0xFFu),
        (uint8_t)((total_bytes >> 8) & 0xFFu),
        (uint8_t)((total_bytes >> 16) & 0xFFu),
        (uint8_t)((total_bytes >> 24) & 0xFFu),
    };
    uint8_t frame[INPUT_FRAME_WIRE_MAX_LEN];
    const size_t len = input_frame_encode_wire(frame, sizeof(frame), INPUT_FRAME_TYPE_IMAGE_END, 0,
                                              0, payload, sizeof(payload));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_encoded_wait(frame, len, timeout_ms);
}

void input_link_send_feedback(const pad_feedback_t *feedback)
{
    if (feedback == NULL) {
        return;
    }
    /* 反馈载荷：左右震动使能、两带强度（低频冲击 + 高频纹理）、玩家灯与
     * 触觉采样；字节 8-15 是两带驱动频率的落地值（Hz，小端 u16 ×4：低频
     * L/R、高频 L/R，PC 侧音频触觉合成按它选频，夹取与回落已在反馈监听者
     * 完成）。老固件的帧只有前 12 字节，PC 按长度判断。 */
    uint8_t payload[16] = {0};
    payload[0] = feedback->rumble_on[PAD_TRIGGER_L2] ? 1u : 0u;
    payload[1] = feedback->rumble_on[PAD_TRIGGER_R2] ? 1u : 0u;
    payload[2] = feedback->rumble_strength[PAD_TRIGGER_L2];
    payload[3] = feedback->rumble_strength[PAD_TRIGGER_R2];
    payload[4] = feedback->player_led;
    payload[5] = feedback->haptic_sample_valid ? feedback->haptic_sample : 0u;
    payload[6] = feedback->rumble_hf_strength[PAD_TRIGGER_L2];
    payload[7] = feedback->rumble_hf_strength[PAD_TRIGGER_R2];
    payload[8] = (uint8_t)(feedback->rumble_lf_freq[PAD_TRIGGER_L2] & 0xFFu);
    payload[9] = (uint8_t)(feedback->rumble_lf_freq[PAD_TRIGGER_L2] >> 8);
    payload[10] = (uint8_t)(feedback->rumble_lf_freq[PAD_TRIGGER_R2] & 0xFFu);
    payload[11] = (uint8_t)(feedback->rumble_lf_freq[PAD_TRIGGER_R2] >> 8);
    payload[12] = (uint8_t)(feedback->rumble_hf_freq[PAD_TRIGGER_L2] & 0xFFu);
    payload[13] = (uint8_t)(feedback->rumble_hf_freq[PAD_TRIGGER_L2] >> 8);
    payload[14] = (uint8_t)(feedback->rumble_hf_freq[PAD_TRIGGER_R2] & 0xFFu);
    payload[15] = (uint8_t)(feedback->rumble_hf_freq[PAD_TRIGGER_R2] >> 8);
    input_link_send_frame(INPUT_FRAME_TYPE_FEEDBACK, 0, payload, sizeof(payload));
}

void input_link_send_out_report(const uint8_t *report, size_t len)
{
    /* 上限按输出报告帧算，不用原始报告的上限：蓝牙 PS 的输出报告 78 字节。 */
    if (!s_running || report == NULL || len == 0 || len > INPUT_FRAME_OUT_MAX_PAYLOAD) {
        return;
    }
    input_link_send_frame(INPUT_FRAME_TYPE_OUT_REPORT, 0, report, len);
}

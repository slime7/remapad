#include "input_link.h"

#include <stdbool.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cli.h"
#include "input_frame.h"
#include "input_source.h"

static const char *TAG = "remapad_input";

/** RX 环形缓冲要能吃下一整帧加一段命令行：太小会在日志刷屏时丢字节。 */
#define INPUT_LINK_RX_BUF 4096
#define INPUT_LINK_TX_BUF 1024
#define INPUT_LINK_TASK_STACK 4096
#define INPUT_LINK_TASK_PRIO 6

static input_frame_rx_t s_rx;
static uint32_t s_frames;

static void on_frame(const input_frame_view_t *frame, void *user)
{
    (void)user;
    if (frame->version != INPUT_FRAME_VERSION) {
        ESP_LOGW(TAG, "frame version %u ignored (expected %u)", frame->version,
                 INPUT_FRAME_VERSION);
        return;
    }
    s_frames++;
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
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            input_frame_rx_feed(&s_rx, buf, (size_t)n, on_frame, on_text, NULL);
        }
    }
}

esp_err_t input_link_start(void)
{
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
    if (xTaskCreate(input_link_task, "remapad-input", INPUT_LINK_TASK_STACK, NULL,
                    INPUT_LINK_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t input_link_frame_count(void)
{
    return s_frames;
}

void input_link_send_feedback(const pad_feedback_t *feedback)
{
    if (feedback == NULL) {
        return;
    }
    /* 反馈载荷：左右震动使能与强度、玩家灯、触觉采样，其余位保留。 */
    uint8_t payload[12] = {0};
    payload[0] = feedback->rumble_on[PAD_TRIGGER_L] ? 1u : 0u;
    payload[1] = feedback->rumble_on[PAD_TRIGGER_R] ? 1u : 0u;
    payload[2] = feedback->rumble_strength[PAD_TRIGGER_L];
    payload[3] = feedback->rumble_strength[PAD_TRIGGER_R];
    payload[4] = feedback->player_led;
    payload[5] = feedback->haptic_sample_valid ? feedback->haptic_sample : 0u;
    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = input_frame_encode(frame, sizeof(frame), INPUT_FRAME_TYPE_FEEDBACK, 0, 0,
                                          payload, sizeof(payload));
    if (len == 0) {
        return;
    }
    /* 主机没在读时直接丢弃，绝不在数据面任务里阻塞。 */
    usb_serial_jtag_write_bytes(frame, len, 0);
}

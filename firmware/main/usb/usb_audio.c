#include "usb_audio.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "haptic_synth.h"
#include "layout.h"
#include "usb_audio_parse.h"

static const char *TAG = "remapad_usbaud";

/** 在途传输数与每个传输的等时包数：3 × 4ms = 12ms 的排队能容忍客户端
 *  任务被调度延迟，也不会占太多 FIFO（等时传输列表一共 64 槽）。 */
#define USB_AUDIO_XFER_COUNT 3
#define USB_AUDIO_ISOC_PACKETS 4
#define USB_AUDIO_SAMPLES_PER_FRAME 48
/** 每毫秒一包：48 样本 × 4ch × 2B = 384 字节，DualSense 的 MPS 392 容得下。 */
#define USB_AUDIO_PACKET_BYTES (USB_AUDIO_SAMPLES_PER_FRAME * HAPTIC_SYNTH_CHANNELS * 2u)
#define USB_AUDIO_DETACH_TIMEOUT_MS 200

static struct {
    portMUX_TYPE mux;

    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t iface;
    uint8_t ep;
    usb_transfer_t *xfer[USB_AUDIO_XFER_COUNT];
    volatile int inflight;
    volatile bool running;
    volatile bool attached;

    haptic_synth_params_t params;
    haptic_synth_state_t synth;
} s_aud = {.mux = portMUX_INITIALIZER_UNLOCKED};

static void audio_xfer_cb(usb_transfer_t *xfer)
{
    if (!s_aud.running || xfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        s_aud.inflight--;
        return;
    }
#if LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG
    unsigned skipped = 0;
    for (int i = 0; i < USB_AUDIO_ISOC_PACKETS; i++) {
        if (xfer->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_SKIPPED) {
            skipped++;
        }
    }
    if (skipped != 0) {
        ESP_LOGD(TAG, "iso packets skipped: %u/%d", skipped, USB_AUDIO_ISOC_PACKETS);
    }
#endif
    haptic_synth_params_t params;
    portENTER_CRITICAL(&s_aud.mux);
    params = s_aud.params;
    portEXIT_CRITICAL(&s_aud.mux);
    haptic_synth_fill(&s_aud.synth, &params, (int16_t *)xfer->data_buffer,
                      USB_AUDIO_ISOC_PACKETS * USB_AUDIO_SAMPLES_PER_FRAME);
    if (usb_host_transfer_submit(xfer) != ESP_OK) {
        s_aud.inflight--;
    }
}

static void fill_xfer_silence(usb_transfer_t *xfer)
{
    memset(xfer->data_buffer, 0, (size_t)xfer->num_bytes);
}

bool usb_audio_attach(usb_host_client_handle_t client, usb_device_handle_t dev,
                      const usb_config_desc_t *cfg, uint16_t vid, uint16_t pid)
{
    if (s_aud.attached) {
        return true;
    }
    /* 只给声明了音频触觉能力的家族接音频通道：Pro 2 也有音频接口，但它的
     * 反馈走同代透传，不该由我们合成。 */
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *layout = pad_layout_find_by_ids(vid, pid, PAD_CONN_USB, &family);
    if (layout == NULL || !layout->out.audio_haptic) {
        return false;
    }
    usb_audio_as_out_t as;
    if (!usb_audio_find_as_out((const uint8_t *)cfg, cfg->wTotalLength, &as)) {
        ESP_LOGD(TAG, "no UAC streaming OUT interface");
        return false;
    }
    if (as.sample_rate_hz != HAPTIC_SYNTH_RATE_HZ || as.channels != HAPTIC_SYNTH_CHANNELS ||
        as.subframe_size != 2 || as.bit_resolution != 16 ||
        as.ep_mps < USB_AUDIO_PACKET_BYTES) {
        ESP_LOGW(TAG, "audio format not drivable: %uch %ubit %uHz mps %u", (unsigned)as.channels,
                 (unsigned)as.bit_resolution, (unsigned)as.sample_rate_hz,
                 (unsigned)as.ep_mps);
        return false;
    }
    if (usb_host_interface_claim(client, dev, as.iface, as.alt) != ESP_OK) {
        ESP_LOGW(TAG, "audio interface %u claim failed", (unsigned)as.iface);
        return false;
    }
    s_aud.client = client;
    s_aud.dev = dev;
    s_aud.iface = as.iface;
    s_aud.ep = as.ep_addr;
    s_aud.inflight = 0;
    /* claim 成功即接管了接口：后面的分配失败也要走 detach 完整收尾。 */
    s_aud.attached = true;
    memset(&s_aud.params, 0, sizeof(s_aud.params));
    haptic_synth_reset(&s_aud.synth);

    bool ok = true;
    for (int i = 0; i < USB_AUDIO_XFER_COUNT && ok; i++) {
        if (usb_host_transfer_alloc(USB_AUDIO_PACKET_BYTES * USB_AUDIO_ISOC_PACKETS,
                                    USB_AUDIO_ISOC_PACKETS, &s_aud.xfer[i]) != ESP_OK) {
            s_aud.xfer[i] = NULL;
            ok = false;
            break;
        }
        usb_transfer_t *xfer = s_aud.xfer[i];
        xfer->device_handle = dev;
        xfer->bEndpointAddress = as.ep_addr;
        xfer->callback = audio_xfer_cb;
        xfer->context = NULL;
        xfer->num_bytes = USB_AUDIO_PACKET_BYTES * USB_AUDIO_ISOC_PACKETS;
        for (int p = 0; p < USB_AUDIO_ISOC_PACKETS; p++) {
            xfer->isoc_packet_desc[p].num_bytes = USB_AUDIO_PACKET_BYTES;
        }
        fill_xfer_silence(xfer);
    }
    if (!ok) {
        ESP_LOGW(TAG, "audio transfer alloc failed");
        usb_audio_detach();
        return false;
    }
    /* 回调只能从客户端任务的 handle_events 里跑：先置 running 再提交。 */
    s_aud.running = true;
    for (int i = 0; i < USB_AUDIO_XFER_COUNT; i++) {
        if (usb_host_transfer_submit(s_aud.xfer[i]) == ESP_OK) {
            s_aud.inflight++;
        }
    }
    ESP_LOGI(TAG, "audio haptics streaming: iface %u alt %u ep 0x%02x mps %u",
             (unsigned)as.iface, (unsigned)as.alt, (unsigned)as.ep_addr,
             (unsigned)as.ep_mps);
    return true;
}

void usb_audio_detach(void)
{
    if (!s_aud.attached) {
        return;
    }
    s_aud.running = false;
    /* 在途传输收尾：flush 强制完成（CANCELED），回调据此停发。 */
    usb_host_endpoint_flush(s_aud.dev, s_aud.ep);
    for (int i = 0; i < USB_AUDIO_DETACH_TIMEOUT_MS / 2 && s_aud.inflight > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    for (int i = 0; i < USB_AUDIO_XFER_COUNT; i++) {
        if (s_aud.xfer[i] != NULL) {
            usb_host_transfer_free(s_aud.xfer[i]);
            s_aud.xfer[i] = NULL;
        }
    }
    usb_host_interface_release(s_aud.client, s_aud.dev, s_aud.iface);
    s_aud.client = NULL;
    s_aud.dev = NULL;
    s_aud.iface = 0;
    s_aud.ep = 0;
    s_aud.inflight = 0;
    s_aud.attached = false;
    ESP_LOGI(TAG, "audio haptics detached");
}

bool usb_audio_streaming(void)
{
    return s_aud.running;
}

void usb_audio_haptic(const haptic_synth_params_t *params)
{
    if (!s_aud.running || params == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_aud.mux);
    s_aud.params = *params;
    portEXIT_CRITICAL(&s_aud.mux);
}

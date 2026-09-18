#include "usb_transport.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "usb_audio.h"
#include "usb_input.h"

static const char *TAG = "remapad_usbhost";

#define USB_LIB_TASK_STACK 4096
#define USB_LIB_TASK_PRIO 5
#define USB_CLIENT_TASK_STACK 4096
#define USB_CLIENT_TASK_PRIO 6
#define USB_CLIENT_POLL_MS 20
/** 原始报告上限：USB HID 报告实际不超过 64 字节，与 PAD_REPORT_MAX 同值。 */
#define USB_REPORT_MAX 64
/** 报告描述符探测长度：用途页与用途固定在最前面几个字节。 */
#define USB_DESC_PROBE_LEN 64

/** HID 用途：Generic Desktop（0x01）下的 Joystick（0x04）与 Game Pad（0x05）。 */
#define USB_HID_USAGE_PAGE_GENERIC_DESKTOP 0x01u
#define USB_HID_USAGE_JOYSTICK 0x04u
#define USB_HID_USAGE_GAMEPAD 0x05u
/** HID 报告描述符的取值类型（GET_DESCRIPTOR 的 wValue 高字节）。 */
#define USB_HID_DESC_TYPE_REPORT 0x22u
/** HID 接口类代码与中断端点类型。 */
#define USB_IFACE_CLASS_HID 0x03u
#define USB_EP_TYPE_INTERRUPT 0x03u

typedef struct {
    portMUX_TYPE mux;

    usb_host_client_handle_t client;
    TaskHandle_t lib_task;
    TaskHandle_t client_task;

    usb_device_handle_t dev;
    bool dev_open;
    uint8_t iface_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t mps_in;
    uint16_t mps_out;

    usb_transfer_t *in_xfer;
    usb_transfer_t *out_xfer;
    volatile bool in_inflight;
    volatile bool out_inflight;
    volatile bool resubmit_in;

    uint8_t out_buf[USB_REPORT_MAX];
    volatile size_t out_len;

    bool lib_installed;
    bool client_registered;
    bool wait_all_free;
    volatile bool running;
} usb_host_ctx_t;

static usb_host_ctx_t s_host = {.mux = portMUX_INITIALIZER_UNLOCKED};

/** 报告描述符开头是否声明了手柄用途（Generic Desktop 下的 Joystick / Game Pad）。 */
static bool report_desc_is_gamepad(const uint8_t *desc, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (desc[i] == 0x05 && desc[i + 1] == USB_HID_USAGE_PAGE_GENERIC_DESKTOP &&
            desc[i + 2] == 0x09 &&
            (desc[i + 3] == USB_HID_USAGE_JOYSTICK || desc[i + 3] == USB_HID_USAGE_GAMEPAD)) {
            return true;
        }
    }
    return false;
}

/** 取报告描述符开头一段（GET_DESCRIPTOR / HID Report）。返回实际字节数。 */
static size_t fetch_report_desc(usb_device_handle_t dev, uint8_t iface_num, uint8_t *out,
                                size_t out_len)
{
    usb_transfer_t *xfer = NULL;
    if (usb_host_transfer_alloc((int)(sizeof(usb_setup_packet_t) + out_len), 0, &xfer) != ESP_OK) {
        return 0;
    }
    xfer->device_handle = dev;
    xfer->bEndpointAddress = 0; /* 控制传输走默认管道 */
    xfer->callback = NULL;
    xfer->context = NULL;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)(USB_HID_DESC_TYPE_REPORT << 8);
    setup->wIndex = iface_num;
    setup->wLength = (uint16_t)out_len;
    xfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + out_len);

    size_t got = 0;
    if (usb_host_transfer_submit_control(s_host.client, xfer) == ESP_OK &&
        xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        got = (size_t)xfer->actual_num_bytes;
        if (got > out_len) {
            got = out_len;
        }
        memcpy(out, xfer->data_buffer + sizeof(usb_setup_packet_t), got);
    }
    usb_host_transfer_free(xfer);
    return got;
}

/** 在一个 HID 接口上探测报告描述符，命中手柄用途时记录端点信息。 */
static bool probe_interface(usb_device_handle_t dev, uint8_t iface_num, uint8_t ep_in,
                            uint8_t ep_out, uint16_t mps_in, uint16_t mps_out)
{
    if (ep_in == 0) {
        return false;
    }
    uint8_t desc[USB_DESC_PROBE_LEN];
    const size_t len = fetch_report_desc(dev, iface_num, desc, sizeof(desc));
    if (len == 0 || !report_desc_is_gamepad(desc, len)) {
        return false;
    }
    s_host.iface_num = iface_num;
    s_host.ep_in = ep_in;
    s_host.ep_out = ep_out;
    s_host.mps_in = mps_in;
    s_host.mps_out = mps_out;
    return true;
}

/**
 * 在配置描述符里挑手柄用途的 HID 接口。复合设备（Pro Controller 2 除 HID 外
 * 还有厂商通道与音频接口）只有 HID 那一个能用，其余接口跳过；报告描述符里
 * 没有手柄用途的 HID 接口（键盘、鼠标）同样跳过。找不到返回 false。
 */
static bool find_gamepad_interface(usb_device_handle_t dev, const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int iface = -1;
    bool hid = false;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint16_t mps_in = 0;
    uint16_t mps_out = 0;

    while (p + 2 <= end) {
        const uint8_t len = p[0];
        const uint8_t type = p[1];
        if (len < 2 || p + len > end) {
            break;
        }
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (hid && probe_interface(dev, (uint8_t)iface, ep_in, ep_out, mps_in, mps_out)) {
                return true;
            }
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;
            iface = intf->bInterfaceNumber;
            hid = intf->bInterfaceClass == USB_IFACE_CLASS_HID;
            ep_in = 0;
            ep_out = 0;
            mps_in = 0;
            mps_out = 0;
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && iface >= 0 && hid) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
            if ((ep->bmAttributes & 0x03u) == USB_EP_TYPE_INTERRUPT) {
                const uint16_t mps = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
                if ((ep->bEndpointAddress & 0x80u) != 0) {
                    ep_in = ep->bEndpointAddress;
                    mps_in = mps;
                } else {
                    ep_out = ep->bEndpointAddress;
                    mps_out = mps;
                }
            }
        }
        p += len;
    }
    /* 末一个接口在循环里还没被探测（下一次 INTERFACE 才会触发），这里补一次。 */
    return hid && probe_interface(dev, (uint8_t)iface, ep_in, ep_out, mps_in, mps_out);
}

static void submit_in(void)
{
    if (s_host.in_xfer == NULL) {
        return;
    }
    uint16_t mps = s_host.mps_in;
    if (mps == 0 || mps > USB_REPORT_MAX) {
        mps = USB_REPORT_MAX;
    }
    s_host.in_xfer->num_bytes = (int)mps;
    s_host.in_inflight = true;
    if (usb_host_transfer_submit(s_host.in_xfer) != ESP_OK) {
        s_host.in_inflight = false;
        ESP_LOGW(TAG, "input transfer submit failed");
    }
}

static void submit_out(void)
{
    uint8_t buf[USB_REPORT_MAX];
    size_t len = 0;
    portENTER_CRITICAL(&s_host.mux);
    len = s_host.out_len;
    if (len > 0) {
        memcpy(buf, s_host.out_buf, len);
        s_host.out_len = 0;
    }
    portEXIT_CRITICAL(&s_host.mux);
    if (len == 0 || s_host.out_xfer == NULL) {
        return;
    }
    memcpy(s_host.out_xfer->data_buffer, buf, len);
    s_host.out_xfer->num_bytes = (int)len;
    s_host.out_inflight = true;
    if (usb_host_transfer_submit(s_host.out_xfer) != ESP_OK) {
        s_host.out_inflight = false;
        ESP_LOGW(TAG, "output transfer submit failed");
    }
}

static void in_transfer_cb(usb_transfer_t *xfer)
{
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        usb_input_submit_report(xfer->data_buffer, (size_t)xfer->actual_num_bytes);
    }
    s_host.in_inflight = false;
    if (s_host.dev_open) {
        /* 续收在客户端任务里提交：回调上下文只置标志。 */
        s_host.resubmit_in = true;
    }
}

static void out_transfer_cb(usb_transfer_t *xfer)
{
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGD(TAG, "output transfer status %d", (int)xfer->status);
    }
    s_host.out_inflight = false;
}

static void close_device(void)
{
    if (!s_host.dev_open) {
        return;
    }
    s_host.dev_open = false;
    /* 音频触觉先收（自己的接口与传输），再等 HID 的在途传输收尾。 */
    usb_audio_detach();
    /* 等在途传输收尾（拔线时由 host 栈以 NO_DEVICE 结束），再释放传输与接口。 */
    for (int i = 0; i < 50 && (s_host.in_inflight || s_host.out_inflight); i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s_host.in_xfer != NULL) {
        usb_host_transfer_free(s_host.in_xfer);
        s_host.in_xfer = NULL;
    }
    if (s_host.out_xfer != NULL) {
        usb_host_transfer_free(s_host.out_xfer);
        s_host.out_xfer = NULL;
    }
    if (s_host.ep_in != 0) {
        usb_host_endpoint_clear(s_host.dev, s_host.ep_in);
    }
    usb_host_interface_release(s_host.client, s_host.dev, s_host.iface_num);
    usb_host_device_close(s_host.client, s_host.dev);
    s_host.dev = NULL;
    s_host.ep_in = 0;
    s_host.ep_out = 0;
    usb_input_note_device(false, 0, 0);
}

static void open_device(uint8_t addr)
{
    if (s_host.dev_open) {
        return;
    }
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(s_host.client, addr, &dev) != ESP_OK || dev == NULL) {
        ESP_LOGW(TAG, "device open failed (addr %u)", (unsigned)addr);
        return;
    }
    const usb_device_desc_t *dev_desc = NULL;
    if (usb_host_get_device_descriptor(dev, &dev_desc) != ESP_OK || dev_desc == NULL) {
        usb_host_device_close(s_host.client, dev);
        return;
    }
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
        usb_host_device_close(s_host.client, dev);
        return;
    }
    if (!find_gamepad_interface(dev, cfg)) {
        ESP_LOGW(TAG, "device %04x:%04x has no HID gamepad interface (skipped)",
                 (unsigned)dev_desc->idVendor, (unsigned)dev_desc->idProduct);
        usb_host_device_close(s_host.client, dev);
        return;
    }
    if (usb_host_interface_claim(s_host.client, dev, s_host.iface_num, 0) != ESP_OK) {
        ESP_LOGW(TAG, "interface %u claim failed", (unsigned)s_host.iface_num);
        usb_host_device_close(s_host.client, dev);
        return;
    }
    s_host.dev = dev;
    s_host.dev_open = true;
    if (usb_host_transfer_alloc(USB_REPORT_MAX, 0, &s_host.in_xfer) != ESP_OK) {
        s_host.in_xfer = NULL;
        close_device();
        return;
    }
    s_host.in_xfer->device_handle = dev;
    s_host.in_xfer->bEndpointAddress = s_host.ep_in;
    s_host.in_xfer->callback = in_transfer_cb;
    s_host.in_xfer->context = NULL;
    if (s_host.ep_out != 0 && usb_host_transfer_alloc(USB_REPORT_MAX, 0, &s_host.out_xfer) == ESP_OK) {
        s_host.out_xfer->device_handle = dev;
        s_host.out_xfer->bEndpointAddress = s_host.ep_out;
        s_host.out_xfer->callback = out_transfer_cb;
        s_host.out_xfer->context = NULL;
    } else {
        s_host.out_xfer = NULL;
    }
    ESP_LOGI(TAG, "hid interface %u claimed (ep in=0x%02x out=0x%02x)", (unsigned)s_host.iface_num,
             (unsigned)s_host.ep_in, (unsigned)s_host.ep_out);
    usb_input_note_device(true, dev_desc->idVendor, dev_desc->idProduct);
    /* 音频触觉通道（DS5 一类带 UAC 触觉的设备）：不成就只是没有音频触觉，
     * HID 反馈照常。 */
    usb_audio_attach(s_host.client, dev, cfg, dev_desc->idVendor, dev_desc->idProduct);
    submit_in();
}

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "device connected (addr %u)", (unsigned)event->new_dev.address);
        open_device(event->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "device disconnected");
        if (s_host.dev_open && event->dev_gone.dev_hdl != s_host.dev) {
            break;
        }
        close_device();
        break;
    default:
        break;
    }
}

static void lib_task(void *param)
{
    (void)param;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            /* 客户端都撤了：让库把设备标成可释放，必要时再等 ALL_FREE。 */
            if (usb_host_device_free_all() == ESP_OK) {
                break;
            }
            s_host.wait_all_free = true;
        }
        if (s_host.wait_all_free && (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0) {
            break;
        }
    }
    s_host.lib_task = NULL;
    vTaskDelete(NULL);
}

static void client_task(void *param)
{
    (void)param;
    while (s_host.running) {
        usb_host_client_handle_events(s_host.client, pdMS_TO_TICKS(USB_CLIENT_POLL_MS));
        if (!s_host.dev_open) {
            continue;
        }
        if (s_host.resubmit_in && !s_host.in_inflight) {
            s_host.resubmit_in = false;
            submit_in();
        }
        if (s_host.out_len > 0 && !s_host.out_inflight) {
            submit_out();
        }
    }
    s_host.client_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t usb_host_start(void)
{
    if (s_host.running) {
        return ESP_OK;
    }
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        /* 只挂片内那一个 FSLS PHY（GPIO19/20，与 USB-Serial/JTAG 复用）。 */
        .peripheral_map = BIT0,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_host.lib_installed = true;
    if (xTaskCreate(lib_task, "remapad-usb", USB_LIB_TASK_STACK, NULL, USB_LIB_TASK_PRIO,
                    &s_host.lib_task) != pdPASS) {
        usb_host_uninstall();
        s_host.lib_installed = false;
        return ESP_ERR_NO_MEM;
    }
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async =
            {
                .client_event_callback = client_event_cb,
                .callback_arg = NULL,
            },
    };
    err = usb_host_client_register(&client_config, &s_host.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        usb_host_stop();
        return err;
    }
    s_host.client_registered = true;
    s_host.running = true;
    if (xTaskCreate(client_task, "remapad-usbc", USB_CLIENT_TASK_STACK, NULL,
                    USB_CLIENT_TASK_PRIO, &s_host.client_task) != pdPASS) {
        usb_host_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "usb host started (otg host mode)");
    return ESP_OK;
}

esp_err_t usb_host_stop(void)
{
    if (!s_host.lib_installed && !s_host.running) {
        return ESP_OK;
    }
    s_host.running = false;
    for (int i = 0; i < 50 && s_host.client_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_host.client_task != NULL) {
        vTaskDelete(s_host.client_task);
        s_host.client_task = NULL;
    }
    close_device();
    if (s_host.client_registered) {
        usb_host_client_deregister(s_host.client);
        s_host.client_registered = false;
        s_host.client = NULL;
    }
    /* 没有客户端后库任务会从 handle_events 返回并退出，随后才能卸库。 */
    for (int i = 0; i < 50 && s_host.lib_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_host.lib_task != NULL) {
        vTaskDelete(s_host.lib_task);
        s_host.lib_task = NULL;
    }
    if (s_host.lib_installed) {
        usb_host_uninstall();
        s_host.lib_installed = false;
    }
    ESP_LOGI(TAG, "usb host stopped");
    return ESP_OK;
}

bool usb_host_running(void)
{
    return s_host.running;
}

void usb_host_queue_output(const uint8_t *report, size_t len)
{
    if (report == NULL || len == 0 || len > USB_REPORT_MAX || !s_host.running) {
        return;
    }
    portENTER_CRITICAL(&s_host.mux);
    memcpy(s_host.out_buf, report, len);
    s_host.out_len = len;
    portEXIT_CRITICAL(&s_host.mux);
}

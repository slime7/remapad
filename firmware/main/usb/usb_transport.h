#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB host 传输层（usb_transport.c）：装栈、枚举、挑 HID 手柄接口、收 IN
 * 报告、写 OUT 报告。切换 PHY 会断开板卡上的 COM 口（USB-Serial/JTAG），
 * 因此进入 host 之前必须先把串口链路与控制台出口迁走，见 usb_role.c。
 *
 * 文件名避开 usb_host.h：那一份是 Registry 组件 espressif/usb 的头文件，
 * 同名头会被本目录遮住。
 *
 * 依赖 Registry 组件 espressif/usb（IDF 6.x 已把 USB Host 库移出核心）。
 */

/** 装 host 栈并起任务；重复调用返回 ESP_OK。 */
esp_err_t usb_host_start(void);

/** 拆 host 栈（先关设备再卸库）；未启动时返回 ESP_OK。 */
esp_err_t usb_host_stop(void);

/** host 栈是否在运行。 */
bool usb_host_running(void);

/** 把一帧输出报告交给 host 任务写进 OUT 端点（覆盖上一帧未发出的）。 */
void usb_host_queue_output(const uint8_t *report, size_t len);

#ifdef __cplusplus
}
#endif

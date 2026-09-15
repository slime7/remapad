#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 控制台输出通道：设备模式下日志与 CLI 走 USB-Serial/JTAG（构建期主控制台，
 * 由 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 决定），切到 USB host 后 USJ 物理口
 * 让给了手柄，日志与 CLI 必须改走 UART0（GPIO43/44 扩展焊盘，接 USB-UART
 * 适配器可读）。因此输出统一经过这里，两个通道共用同一份命令解析。
 */

/** 把一段文本写到当前通道（未切 UART0 时走 stdout，即 USJ 的非阻塞 vfs）。 */
void console_out_write(const char *text, size_t len);

/** 日志与 CLI 输出切到 UART0 并起读任务（喂 CLI 行解析）。重复调用无副作用。 */
esp_err_t console_out_use_uart0(void);

/** 日志与 CLI 输出切回 USB-Serial/JTAG（卸掉 UART0 驱动与读任务）。 */
void console_out_use_usj(void);

/** 当前是否走 UART0。 */
bool console_out_uart_active(void);

#ifdef __cplusplus
}
#endif

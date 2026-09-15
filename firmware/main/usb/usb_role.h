#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB 角色切换（运行时）：切到「手柄」时先把日志与 CLI 出口换到 UART0、
 * 放掉 USB-Serial/JTAG，再装 USB host 栈（安装时把 PHY 切到 OTG host），
 * PC 上的 COM 口随之消失；切回串口做反向操作，COM 口回来。角色只对本次
 * 运行生效、不写 NVS，复位后 PHY 回默认的 USB-Serial/JTAG。
 */

/** 切到「手柄（USB host）」；失败时回滚到串口角色。 */
esp_err_t usb_role_enter_host(void);

/** 切回「串口（USB-Serial/JTAG）」。 */
esp_err_t usb_role_leave_host(void);

/** 当前是否处于 host 角色。 */
bool usb_role_host_active(void);

#ifdef __cplusplus
}
#endif

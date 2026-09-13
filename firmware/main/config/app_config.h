#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用户设置持久化（NVS 命名空间 "remapad"，键 "cfg"）：背光亮度、手柄身份
 * 配置（类型 + 机身配色）与上报固件版本。USB 连接模式只在内存中生效、
 * 不落盘，开机恒为串口。内存表在 app_config_init 时读入，setter 只改内存
 * 表并投递快照；落盘由内部 RAM 栈的提交任务执行（与 ble_creds 同一模式：
 * owner task 栈在 PSRAM，flash 写入的禁缓存窗口内访问 PSRAM 栈会触发 cache
 * 异常重启，任何任务上下文都不得直接写 flash）。
 */

/** USB 连接模式。桥接（otg）双端禁切；该值不持久化，重启回到 device。 */
typedef enum {
    APP_CONFIG_USB_DEVICE = 0,
    APP_CONFIG_USB_HOST = 1,
} app_config_usb_role_t;

/** 手柄形态：Pro（默认）或 JoyCon 组合。 */
typedef enum {
    APP_CONFIG_CTRL_PRO = 0,
    APP_CONFIG_CTRL_JOYCON = 1,
} app_config_ctrl_type_t;

typedef struct {
    /** 背光亮度 0-100；0 仅在息屏时出现，开机下限由调用方保证。 */
    uint8_t brightness;
    /** 息屏状态：息屏时背光 0，亮屏恢复 brightness。 */
    bool screen_on;
    /** USB 角色（app_config_usb_role_t）：仅本次运行有效。 */
    uint8_t usb_role;
    uint8_t ctrl_type; /* app_config_ctrl_type_t */
    /** 机身 / 按键 / 握把配色 0xRRGGBB，0 表示未设置（沿用出厂占位）。 */
    uint32_t body_color;
    uint32_t button_color;
    uint32_t grip_color;
    /**
     * 上报给主机的手柄固件版本（主.次.修订），0x10 版本查询、0x7E40 与
     * 0x13000 出厂块的版本字段共用。默认 1.6.1；主机的固件更新推送由
     * 假升级会话接收，完成后递增并落盘（假装升级到新版本）。
     */
    uint8_t fw_version[3];
} app_config_t;

/** 读入 NVS 配置到内存表（无记录时用默认值）。须在 nvs_init 之后调用。 */
esp_err_t app_config_init(void);

/** 当前配置（内存表只读视图）。 */
const app_config_t *app_config_get(void);

void app_config_set_brightness(uint8_t pct);
void app_config_set_screen_on(bool on);
/** 只改运行时角色（不落盘）：重启后回到串口。 */
void app_config_set_usb_role(app_config_usb_role_t role);
void app_config_set_controller(app_config_ctrl_type_t type,
                               uint32_t body_rgb, uint32_t button_rgb, uint32_t grip_rgb);

/** 覆盖上报固件版本（假升级完成时递增），立即落盘。 */
void app_config_set_fw_version(const uint8_t ver[3]);

#ifdef __cplusplus
}
#endif

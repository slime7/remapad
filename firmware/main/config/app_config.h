#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 上报给主机的手柄固件版本（主.次.修订）的出厂值。主机（Switch 2）拿它判断
 * 要不要推手柄固件更新：版本偏低时会弹更新提示、并在「更新手柄」菜单里
 * 推整包。想固定上报版本就改这三个常量并连固件一起刷；串口 fwver 写的值存在
 * NVS 里、优先于这里的出厂值（清除 NVS 后回到出厂值）。
 */
#define CONFIG_DEFAULT_FW_VERSION_MAJOR 9u
#define CONFIG_DEFAULT_FW_VERSION_MINOR 9u
#define CONFIG_DEFAULT_FW_VERSION_REVISION 9u

/**
 * 用户设置持久化（NVS 命名空间 "remapad"，键 "cfg"）：背光亮度、手柄配色、DS 行为与上报固件版本。
 * setter 只改内存表并置脏标记，落盘由内部 RAM 栈的提交任务每 1 分钟检查一次、确有改动才写一次；
 * flash 写入的禁缓存窗口内不能访问 PSRAM 栈，任何任务都不得直接写 flash。USB 连接模式只在内存中生效。
 */

/** USB 连接模式：device = 端口给 PC 串口，host = 端口给 OTG host 直插手柄。
 *  该值不持久化，重启回到 device。 */
typedef enum {
    APP_CONFIG_USB_DEVICE = 0,
    APP_CONFIG_USB_HOST = 1,
} app_config_usb_role_t;

typedef struct {
    /** 背光亮度 0-100；0 仅在息屏时出现，开机下限由调用方保证。 */
    uint8_t brightness;
    /** 息屏状态：息屏时背光 0，亮屏恢复 brightness。 */
    bool screen_on;
    /** USB 角色（app_config_usb_role_t）：仅本次运行有效。 */
    uint8_t usb_role;
    /** 机身 / 按键 / 高光 / 握把配色 0xRRGGBB，0 表示未设置（沿用出厂占位）。
     *  四段与出厂块 0x13019 起的布局一一对应。 */
    uint32_t body_color;
    uint32_t button_color;
    uint32_t accent_color;
    uint32_t grip_color;
    /**
     * 上报给主机的手柄固件版本（主.次.修订），0x10 版本查询、0x7E40 与
     * 0x13000 出厂块的版本字段共用。出厂值见 CONFIG_DEFAULT_FW_VERSION_*；
     * 主机的固件更新推送由假升级会话接收并逐帧应答（取舍见 ADR 0032），
     * 是否重启伪装成「已升级」由串口 fwapply 一次性武装。
     */
    uint8_t fw_version[3];
    /**
     * DS4 / DS5 手柄行为（见 pad/ds_behavior.h）：触摸板映射加减键（默认关）、
     * 触摸板按下发截图（默认开）。
     */
    bool ds_touchpad_plus_minus;
    bool ds_capture_key;
} app_config_t;

/** 读入 NVS 配置到内存表（无记录时用默认值）。须在 nvs_init 之后调用。 */
esp_err_t app_config_init(void);

/** 当前配置（内存表只读视图）。 */
const app_config_t *app_config_get(void);

void app_config_set_brightness(uint8_t pct);
void app_config_set_screen_on(bool on);
/** 只改运行时角色（不落盘）：重启后回到串口。 */
void app_config_set_usb_role(app_config_usb_role_t role);
void app_config_set_controller_colors(uint32_t body_rgb, uint32_t button_rgb,
                                      uint32_t accent_rgb, uint32_t grip_rgb);

/** 覆盖上报固件版本（假升级完成时递增），随周期检查落盘。 */
void app_config_set_fw_version(const uint8_t ver[3]);

/** DS 手柄行为两项开关（默认：触摸板映射关、截图键开）。 */
void app_config_set_ds_behavior(bool touchpad_plus_minus, bool capture_key);

/** 立即叫醒提交任务落盘（默认每 1 分钟检查一次）。用于「改完就要重启」的
 * 场景：不落盘直接重启会丢掉刚改的版本。 */
void app_config_flush(void);

#ifdef __cplusplus
}
#endif

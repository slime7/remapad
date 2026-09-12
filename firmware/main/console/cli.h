#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 串口控制台 CLI（USB-Serial/JTAG 复用烧录/日志口）：以行命令驱动设备，
 * 免去验收时手点屏幕。命令走产品控制面同一路径（js_bridge 外部队列或
 * 直接调用安全接口），不引入第二条控制逻辑。
 *
 * 可用命令（回车结尾，回复为单行文本）：
 *   help / ping / status
 *   key a|home|lr        调试注入按键（lr 为配对 L+R）
 *   backlight 0-100      背光并持久化
 *   screen on|off        息屏 / 亮屏
 *   mode device|host     连接模式（桥接 otg 由 bridge 拒绝）
 *   pairing start|stop   配对模式开关
 *   reboot               软重启（回 COM 模式）
 *
 * 注意：与 idf.py monitor 共用端口时，监视器会抢读输入，二者勿同时使用。
 */

esp_err_t remapad_cli_start(void);

#ifdef __cplusplus
}
#endif

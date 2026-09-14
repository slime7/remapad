#pragma once

#include <stddef.h>
#include <stdint.h>

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
 *   version              运行镜像版本 / 分区 / OTA 会话状态
 *   rollback             回滚到上一个可用镜像（仅待验证状态有效）
 *   reboot               软重启（回 COM 模式）
 *
 * 注意：与 idf.py monitor 共用端口时，监视器会抢读输入，二者勿同时使用。
 */

esp_err_t remapad_cli_start(void);

/**
 * 把串口上的一串字节喂给行解析：input_link 从同一根 USJ 上读到的非帧字节
 * 由这里接住，因此桥接数据与命令行共用一条链路。在接收任务上下文里同步
 * 分发命令，命令本身不做阻塞等待。
 */
void cli_feed_bytes(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

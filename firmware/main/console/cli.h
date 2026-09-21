#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 串口控制台 CLI（与烧录/日志共用 USB-Serial/JTAG）：以行命令驱动设备，走产品控制面同一路径，
 * 不引入第二条控制逻辑。命令清单见 docs/GETTING-STARTED.md 与运行中的 `help`；
 * 与 idf.py monitor 共用端口时监视器会抢读输入，二者勿同时使用。
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

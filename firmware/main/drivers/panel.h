#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ST7789V2 面板：SPI2 总线（只写，无 MISO）、panel IO、内置 ST7789
 * 驱动与面板方向配置。成功后面板处于显示开启状态，背光由 backlight 模块单独
 * 控制，保持熄灭直到调用方显式点亮。 */
esp_err_t panel_init(void);

/** 把一段 RGB565 像素（小端）同步写入面板窗口 [x, x+width) × [y, y+height)：
 * 提交后阻塞到 DMA 完成。像素缓冲会被原地改为 SPI 线序的大端字节，缓冲必须
 * 保持 64 字节对齐且位于 PSRAM，以便 SPI DMA 直接读取。x/y 使用逻辑视口坐标，
 * 面板自身的 20 行 GRAM 偏移由内置驱动通过 set_gap 处理。 */
esp_err_t panel_transfer(uint16_t *pixels, int x, int y, int width, int height);

/** 异步版本：换好字节序、把窗口排进 SPI 队列就返回，不等 DMA 结束，并通过
 * out_seq 交回本笔提交的完成序号。缓冲在 panel_wait_seq 等到该序号之前属于
 * DMA，期间改写会让画面出现黑线或错位；spi_master 按提交顺序完成事务，序号
 * 因此可以用作「这块缓冲什么时候能再用」的判据。 */
esp_err_t panel_transfer_async(uint16_t *pixels, int x, int y, int width, int height,
                               uint32_t *out_seq);

/** 等到指定序号的传输结束；序号已过或没有在飞传输时立即返回。 */
esp_err_t panel_wait_seq(uint32_t seq, uint32_t timeout_ms);

/** 等待所有已提交的异步传输结束；没有在飞的传输时立即返回。超时按传输失败
 * 处理并返回 ESP_ERR_TIMEOUT，此时调用方应放弃当前帧。 */
esp_err_t panel_transfer_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

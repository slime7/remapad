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

/** 把一段 RGB565 像素（小端）写入面板窗口 [x, x+width) × [y, y+height)。
 * 像素缓冲会被原地改为 SPI 线序的大端字节，缓冲必须保持 64 字节对齐且位于
 * PSRAM，以便 SPI DMA 直接读取。x/y 使用逻辑视口坐标，面板自身的 20 行
 * GRAM 偏移由内置驱动通过 set_gap 处理。 */
esp_err_t panel_transfer(uint16_t *pixels, int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif

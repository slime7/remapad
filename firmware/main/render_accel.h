#pragma once

#include "pocketjs/render_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** S3 没有 PPA（那是 P4 的像素加速器），渲染器的加速回调由本机整数实现接管。
 * 接管的收益不只是省掉通用软件光栅里每像素三次除以 255 的合成：渲染器在
 * 字形与纹理走「建 A8 掩码再交给加速器混合」这条路时，没有加速器会先把掩码
 * 建好、再整块回退给软件光栅重画一遍，等于每个像素做两遍。
 *
 * 实现与官方 ESP32-P4 PPA 适配层（hosts/esp-idf/components/pocketjs_esp32p4_ppa）
 * 同一套 ABI：矩形是相对 strip 表面的物理像素坐标，mask 与 destination 共用
 * width 步长，合成结果必须与 renderer 的软件路径逐像素一致。 */
const pocketjs_rgb565_accelerator_t *render_accel(void);

#ifdef __cplusplus
}
#endif

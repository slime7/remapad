#pragma once

#include "pocketjs/render_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** S3 没有 PPA（P4 的像素加速器），渲染器的加速回调由本机整数实现接管：填充、A8 掩码混合与
 *  PSM5650 直拷三条路径，与官方 P4 PPA 适配层同一套 ABI——矩形是相对 strip 表面的物理像素坐标，
 *  mask 与 destination 共用 width 步长，合成结果必须与 renderer 软件路径逐像素一致。 */
const pocketjs_rgb565_accelerator_t *render_accel(void);

#ifdef __cplusplus
}
#endif

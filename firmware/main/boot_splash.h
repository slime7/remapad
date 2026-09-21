#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动画面：PocketJS UI 就绪前由固件自绘的画面与阶段进度条（面板、触摸与背光的初始化由调用方负责；
 *  UI 就绪后由调用方 boot_splash_end 停表，画面归 PocketJS 渲染路径所有）。
 *  begin 之外的所有函数在未 begin 或已 end 时都是空操作。 */

/** 启动阶段预计耗时（毫秒）表：数量须与调用方的阶段表一致，用于把进度条
 *  的时间轴拉开（长阶段持续前进，短阶段不会独占画面）。 */
esp_err_t boot_splash_begin(uint8_t brightness_pct, const uint32_t *stage_ms, size_t stage_count);

/** 推进到第 step 个阶段（从 1 计）：锚点取权重表，阶段内由动画任务填充。 */
void boot_splash_progress(int step);

/** UI 初始化失败时的收尾：进度条改错误色并留在屏上（缓冲保持到复位）。 */
void boot_splash_fail(void);

/** 停止启动画面（UI 初始化成功后调用），缓冲由动画任务退出时释放。 */
void boot_splash_end(void);

#ifdef __cplusplus
}
#endif

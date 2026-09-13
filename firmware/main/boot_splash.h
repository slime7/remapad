#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动画面：PocketJS UI 就绪前由固件自绘的画面与阶段进度条。
 *
 * 面板、触摸与背光的初始化仍由调用方负责；本模块在面板就绪后接管这一段
 * 显示——先整帧铺满背景、几何标记与进度条并点亮背光，之后每次启动阶段推进
 * 只重画按键点与进度条两个小区域。UI 首帧提交成功后由调用方 boot_splash_end
 * 释放资源，画面从此归 PocketJS 渲染路径所有。
 *
 * begin/end 之外的所有函数在未 begin 或已 end 时都是空操作。 */
esp_err_t boot_splash_begin(uint8_t brightness_pct);

/** 推进启动进度：step 从 1 计到 total，进度条按比例填充。 */
void boot_splash_progress(int step, int total);

/** UI 初始化失败时的收尾：进度条改错误色留在屏上，并释放缓冲。 */
void boot_splash_fail(void);

/** 释放启动画面缓冲（UI 首帧提交成功后调用）。 */
void boot_splash_end(void);

#ifdef __cplusplus
}
#endif

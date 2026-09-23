#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Slint 屏幕 UI 的 owner task：面板、触摸、启动画面、Slint 事件循环与每 50 ms
 * 的状态聚合都跑在这一个任务上。
 */
esp_err_t remapad_slint_start(void);

/** 请求一次实机截图（串口 shot 命令）：下一轮状态轮询把整屏画面回传给 PC。 */
void remapad_ui_request_shot(void);

/** 请求一次实时内存全景（串口 mem 命令）：内部堆与 PSRAM 余量打到控制台出口。 */
void remapad_ui_request_mem(void);

/** trace 命令不带帧数时的追踪长度。 */
#define REMAPAD_UI_TRACE_FRAMES_DEFAULT 60U

/** 请求逐帧渲染统计（串口 trace 命令）：接下来 frames 帧每帧一行渲染/提交耗时。 */
void remapad_ui_request_trace(unsigned frames);

#ifdef __cplusplus
}
#endif

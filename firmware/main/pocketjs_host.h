#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the product-owned PocketJS owner task: package, guest, UI core,
 * binding, renderer, and the UI turn loop all run on that one task. */
esp_err_t remapad_pocketjs_start(void);

/**
 * 请求一次实机截图（调试通路）：owner task 在下一帧把整幅画面按整屏渲染一遍，
 * 经 input_link 的图像帧回传给 PC（pc/remapadctl.py 落地成 PNG）。请求只是
 * 一个标志位，串口 CLI 可以安全地在别的任务上调用；PC 链路不在运行时请求被丢弃。
 */
void remapad_ui_request_shot(void);

/**
 * 请求一次实时内存全景（串口 mem 命令）：owner task 在下一帧把 PSRAM/内部堆
 * 余量、QuickJS 记账与对象计数写到控制台出口。JS_ComputeMemoryUsage 是全堆
 * 遍历，只能与 guest 同任务执行，所以这里只置标志位（同截图请求）。
 */
void remapad_ui_request_mem(void);

/** trace 命令不带帧数时的追踪长度：1 秒（30 Hz tick 一帧一行）。 */
#define REMAPAD_UI_TRACE_FRAMES_DEFAULT 30U

/**
 * 请求逐帧 damage 追踪（串口 trace 命令）：owner task 在接下来的 frames 帧里
 * 每帧打一行 damage 计划（region 矩形、折带数、是否整屏重画）与逐条行带的
 * 矩形和耗时，用来量切页、动画这类局部更新的真实代价；frames 传 0 取默认长度。
 */
void remapad_ui_request_trace(unsigned frames);

/**
 * 请求把下一帧的 draw list 原样打到控制台（串口 drawlist 命令）：每行一个
 * 字偏移加八个十六进制字，供 PC 侧离线复算差分与统计 op 构成。输出量在
 * 数十 KB 量级、串口按 115200 收，期间 UI 任务会阻塞在写日志上，因此只做
 * 一次性诊断；owner task 消费标志位后自动清位。
 */
void remapad_ui_request_draw_list(void);

#ifdef __cplusplus
}
#endif

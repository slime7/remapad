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

#ifdef __cplusplus
}
#endif

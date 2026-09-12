#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 产品控制面桥接：PocketJS UI（guest）与固件原生侧的低频命令/事件通道。
 *
 * 传输路径：guest 调 globalThis.__nativeBridge.postMessage(json)（由
 * pocketjs_host 注入的 native surface）→ js_bridge_enqueue 入队 → owner
 * task 每帧在 js_bridge_service 里出队、用 cJSON 分发处理，并通过
 * pocketjs_guest_eval 调用 __onNativeBridgeMessage(json) 回发应答/事件。
 * 入队与出队都发生在 PocketJS owner task 上，无锁。
 *
 * USB 高频输入、NS2 编码与 BLE 数据面不经过此通道（见 docs/ARCHITECTURE.md）。
 */

/** 初始化桥接内部状态；在 owner task 启动前调用一次。 */
esp_err_t js_bridge_init(void);

/** guest 创建后绑定事件回发通道；绑定前事件只记日志。 */
void js_bridge_attach(pocketjs_guest_t *guest);

/** native surface 回调：把 guest 发来的命令 JSON 入队（owner task 上下文）。 */
esp_err_t js_bridge_enqueue(const char *cmd_json);

/** owner task 每帧调用：驱动配对状态机定时流转并处理命令队列。 */
void js_bridge_service(void);

#ifdef __cplusplus
}
#endif

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
 * task 每帧在 js_bridge_service 里出队、按固定键名分发处理，并通过
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

/** 外部任务（PWR 按键 / 串口 CLI）提交命令 JSON：拷入队列，由 owner task
 *  在 js_bridge_service 里走同一分发路径；不阻塞调用方。 */
esp_err_t js_bridge_submit_command(const char *cmd_json);

/** PWR 长按的连接键：有链路或正在广播就提交「停止广播」，否则提交「连接」。
 *  UI 的连接按钮按同一规则在两侧各自推导，两条入口走同一条命令路径。 */
void js_bridge_connect_key(void);

/** 外部任务向 UI 广播事件 JSON：经队列由 owner task 回发给 guest。 */
void js_bridge_post_event(const char *event_json);

/** owner task 每帧调用：驱动配对状态机定时流转并处理命令队列。 */
void js_bridge_service(void);

/** 设置背光并持久化（UI 命令与串口 CLI 共用；0-100）。 */
void js_bridge_set_brightness(int brightness);

/** 息屏 / 亮屏（PWR 键与串口 CLI 共用）：状态落盘并向 UI 广播。 */
void js_bridge_screen_power(bool on);

/** 当前 UI 六态配对状态字符串（串口 CLI status 用）。 */
const char *js_bridge_pairing_state(void);

#ifdef __cplusplus
}
#endif

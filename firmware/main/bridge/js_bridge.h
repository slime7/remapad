#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化预留的产品控制面桥接。
 *
 * 该接口当前不在 ESP-IDF target 的编译源中，也没有连接 PocketJS UI。
 * 后续 USB 输入、NS2 控制器状态和 BLE 配对控制可复用此边界。
 */
esp_err_t js_bridge_init(void);

/**
 * 处理产品控制面命令；当前实现仅为占位调试接口。
 */
void js_bridge_handle_cmd(const char *cmd_json);

/**
 * 发布产品控制面事件；当前实现仅记录日志。
 */
void js_bridge_post_event(const char *event_json);

#ifdef __cplusplus
}
#endif

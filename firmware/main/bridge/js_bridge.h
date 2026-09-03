#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 JavaScript - C 原生桥接调度器
 */
esp_err_t js_bridge_init(void);

/**
 * 处理来自前端 JavaScript 调用的控制命令 (JSON 格式，符合 DeviceCmd 规范)
 * @param cmd_json 包含 t, id 等字段的 JSON 字符串
 */
void js_bridge_handle_cmd(const char *cmd_json);

/**
 * 向前端 JavaScript 发送异步事件通知 (符合 DeviceMsg 规范)
 * @param event_json 事件 JSON 字符串
 */
void js_bridge_post_event(const char *event_json);

#ifdef __cplusplus
}
#endif

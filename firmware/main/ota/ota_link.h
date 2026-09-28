#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "input_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA 升级通道的桥接帧适配（本工程装配，ota_session 核心的 port 实现）：
 * input_link 收到的 OTA 帧翻译成会话消息转交核心，ACK 经桥接链路等待式回发，
 * 阶段/进度以 otaProgress JSON 事件广播给界面。换传输或换业务只重写本层，
 * ota_proto / ota_session 不动。
 */

/** 该帧类型是否属于 OTA 接收通道（BEGIN / DATA / END）。 */
bool ota_link_is_frame_type(uint8_t type);

/** input_link 分派入口：翻译成会话消息后转交核心（非阻塞）。 */
void ota_link_handle_frame(const input_frame_view_t *frame);

/** 装配 port 并启动会话核心。须在 input_link_start 之前调用。 */
esp_err_t ota_link_start(void);

#ifdef __cplusplus
}
#endif

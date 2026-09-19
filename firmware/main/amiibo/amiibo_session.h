#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * amiibo 上传的桥接帧入口（input_link 在帧分派时调用）：接收 PC 发来的
 * BEGIN/DATA/END 帧，逐帧回 ACK，收齐后经 amiibo_store 落库。会话超时在
 * 下一帧到达时顺带清理，不需要独立任务（镜像固定 540 字节、逐帧应答，
 * 与 OTA 的队列 + 任务形态不同）。
 */

/** 这几个帧类型归 amiibo 会话管（0x40-0x43）。 */
bool amiibo_session_is_frame_type(uint8_t type);

/** 处理一帧 amiibo 上传帧并回 ACK（input_link 任务上下文）。 */
void amiibo_session_handle_frame(const input_frame_view_t *frame);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input_frame.h"

/**
 * 桥接输入源（接收段的设备侧）：把 input_link 解码出来的接入/报告/断开帧
 * 变成私有手柄状态，供 dp_task 采样。没有设备接入时输出静置状态，因此 PC
 * 侧拔线不会留下卡住的按键。
 */

/** 注册桥接输入源（在 dp_plane_start 里调用，须早于合成源）。 */
void input_source_register(void);

/** 处理一帧桥接帧（由 input_link 的解帧回调调用）。 */
void input_source_handle_frame(const input_frame_view_t *frame);

/** 当前是否已接入设备。 */
bool input_source_attached(void);

/** 已接收的报告帧数（诊断）。 */
uint32_t input_source_report_count(void);

/** 设备描述（家族 / VID:PID / 连接 / 报告长度），未接入时为 "none"。 */
const char *input_source_device_desc(void);

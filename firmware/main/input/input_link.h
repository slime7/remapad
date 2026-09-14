#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "pad_state.h"

/**
 * 桥接链路（接收段的传输侧）：USB-Serial/JTAG 上的唯一读取者。安装 USJ
 * 驱动后把收到的字节流解成桥接帧与非帧文本：输入帧交给 input_source，
 * OTA 帧交给 ota_session，PING 在这里直接应答，文本交给 CLI 行解析，因此
 * 桥接数据、升级数据与调试命令行共用同一根 Type-C。
 */

/** 启动链路（安装驱动 + 起接收任务）。须在 CLI 就绪之后调用。 */
esp_err_t input_link_start(void);

/** 已解析的桥接帧数（诊断）。 */
uint32_t input_link_frame_count(void);

/**
 * 往 PC 发一帧（反馈、PING 应答与 OTA 应答共用）。主机没在读时丢弃，
 * 在任何任务上下文调用都不会阻塞。
 */
void input_link_send_frame(uint8_t type, uint8_t slot, const uint8_t *payload,
                           size_t payload_len);

/** 回发一帧反馈给 PC（主机 → 手柄方向）：本轮 PC 端只打印，投递到手柄
 *  在后续里程碑实现。 */
void input_link_send_feedback(const pad_feedback_t *feedback);

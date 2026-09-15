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

/**
 * 释放链路（停接收任务 + 卸 USB-Serial/JTAG 驱动）：切到 USB host 前必须
 * 让出这个物理口。切回串口时重新调用 input_link_start。
 */
void input_link_stop(void);

/** 链路是否在运行（host 模式下为 false）。 */
bool input_link_active(void);

/** 已解析的桥接帧数（诊断）。 */
uint32_t input_link_frame_count(void);

/**
 * 往 PC 发一帧（反馈、PING 应答与 OTA 应答共用）。主机没在读时丢弃，
 * 在任何任务上下文调用都不会阻塞。
 */
void input_link_send_frame(uint8_t type, uint8_t slot, const uint8_t *payload,
                           size_t payload_len);

/**
 * 同上，但在 timeout_ms 内重试把整帧推进发送环：串口上的日志流量会占满缓冲，
 * 升级应答这类控制帧不能像数据面那样随手丢。仍可能在超时后放弃，不无限阻塞。
 */
esp_err_t input_link_send_frame_wait(uint8_t type, uint8_t slot, const uint8_t *payload,
                                    size_t payload_len, uint32_t timeout_ms);

/** 回发一帧反馈给 PC（主机 → 手柄方向）：本轮 PC 端只打印，投递到手柄
 *  在后续里程碑实现。 */
void input_link_send_feedback(const pad_feedback_t *feedback);

/**
 * 把编码好的输出报告发给 PC（设备 → PC），由 PC 侧写进手柄：反馈的布局
 * 知识只在固件里有一份，PC 只是搬运。未接入或链路停用时丢弃。
 */
void input_link_send_out_report(const uint8_t *report, size_t len);

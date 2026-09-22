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

/** PC 是否连在串口上：USB-Serial/JTAG 在收主机的 SOF 包（插充电宝不算）。
 *  状态由 IDF 的 USJ 连接监视器维护，控制台选 USJ 时随驱动一并链接。 */
bool input_link_pc_connected(void);

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

/**
 * 回发一帧反馈给 PC（主机 → 手柄方向）：载荷由 pad_feedback_wire 编码
 * （16 字节基础段，或带 HD 子帧的 57 字节），映射知识只在固件里有一份，
 * 这里只搬运。未接入或链路停用时丢弃。
 */
void input_link_send_feedback(const uint8_t *payload, size_t payload_len);

/**
 * 把编码好的输出报告发给 PC（设备 → PC），由 PC 侧写进手柄：反馈的布局
 * 知识只在固件里有一份，PC 只是搬运。未接入或链路停用时丢弃。
 */
void input_link_send_out_report(const uint8_t *report, size_t len);

/**
 * 采集帧（设备 → PC，INPUT_FRAME_TYPE_HOST_RAW）：主机输出的原始字节，
 * 载荷最长到线格式上限（255 字节），因此走线格式编码；slot 传设备侧
 * 记录号，PC 靠它检测跳号丢包。未接入或链路停用时丢弃，不阻塞。
 */
void input_link_send_host_raw(uint8_t slot, const uint8_t *payload, size_t payload_len);

/**
 * 实机截图通路（设备 → PC）：INFO 声明尺寸与像素格式，DATA 按偏移分块回传
 * 像素，END 汇报总字节数。三者都用等待式发送并可能超时，供 UI owner task
 * 在调试命令里同步回传整幅画面；PC 侧按偏移是否覆盖满判定完整性。
 * 链路未运行时返回 ESP_ERR_INVALID_STATE，参数越界返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t input_link_send_image_info(uint16_t width, uint16_t height, uint32_t timeout_ms);

/** 偏移单位是整幅画面的字节偏移（行序自上而下、每像素 2 字节，小端）。 */
esp_err_t input_link_send_image_data(uint32_t offset, const uint8_t *data, size_t len,
                                    uint32_t timeout_ms);

esp_err_t input_link_send_image_end(uint32_t total_bytes, uint32_t timeout_ms);

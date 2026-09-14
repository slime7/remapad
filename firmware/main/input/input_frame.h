#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 桥接帧协议（PC → 设备）：与 USB-Serial/JTAG 上的 CLI 文本共用一条字节流，
 * 靠同步字区分。
 *
 * 帧布局（长度不含自身）：
 *   A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
 * CRC-16/CCITT-FALSE 覆盖整帧除末尾两字节外的全部字节（含同步字）。
 * 载荷上限 72 字节：8 字节设备标识 + 单帧最多 64 字节原始报告。
 */
#define INPUT_FRAME_SYNC0 0xA5u
#define INPUT_FRAME_SYNC1 0x5Au
#define INPUT_FRAME_VERSION 0x01u
#define INPUT_FRAME_HEADER_LEN 7u
#define INPUT_FRAME_CRC_LEN 2u
#define INPUT_FRAME_MAX_PAYLOAD 72u
#define INPUT_FRAME_MAX_LEN \
    (INPUT_FRAME_HEADER_LEN + INPUT_FRAME_MAX_PAYLOAD + INPUT_FRAME_CRC_LEN)

/** 帧类型。 */
typedef enum {
    INPUT_FRAME_TYPE_ATTACH = 0x01,   /**< 载荷 = 设备标识（8 字节）。 */
    INPUT_FRAME_TYPE_DETACH = 0x02,   /**< 载荷 = 设备标识（8 字节）。 */
    INPUT_FRAME_TYPE_REPORT = 0x10,   /**< 载荷 = 设备标识 + 原始报告。 */
    INPUT_FRAME_TYPE_FEEDBACK = 0x20, /**< 载荷 = 反馈（设备 → PC）。 */
    INPUT_FRAME_TYPE_PING = 0x7F,     /**< 载荷 = 版本号（1 字节）。 */
} input_frame_type_t;

/** 设备标识载荷：家族、连接方式、VID/PID 与报告标识（8 字节小端）。 */
#define INPUT_DEVICE_ID_LEN 8u

/** 解析出的一帧视图（payload 指向调用方缓冲，不复制）。 */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t slot;
    uint8_t seq;
    size_t payload_len;
    const uint8_t *payload;
} input_frame_view_t;

/** CRC-16/CCITT-FALSE（多项式 0x1021，初值 0xFFFF）。校验值 "123456789" = 0x29B1。 */
uint16_t input_frame_crc16(const uint8_t *data, size_t len);

/**
 * 编码一帧到 out。返回整帧长度；缓冲不足或载荷超限返回 0。
 * payload 可为 NULL（len 为 0 时）。
 */
size_t input_frame_encode(uint8_t *out, size_t out_len, uint8_t type, uint8_t slot,
                          uint8_t seq, const uint8_t *payload, size_t payload_len);

/** 解帧状态机：把字节流切成桥接帧与其余文本（CLI 行命令）。 */
typedef struct {
    uint8_t buf[INPUT_FRAME_MAX_LEN + 16u];
    size_t len;
} input_frame_rx_t;

typedef void (*input_frame_cb_t)(const input_frame_view_t *frame, void *user);
typedef void (*input_text_cb_t)(const uint8_t *text, size_t len, void *user);

/** 复位解帧状态（清空残留字节）。 */
void input_frame_rx_reset(input_frame_rx_t *rx);

/**
 * 喂入一段字节：每识别出一帧调用 on_frame，其余字节原样交给 on_text（可能
 * 分多次调用）。校验失败或长度越界时丢弃同步字并继续扫描，不会读越界。
 */
void input_frame_rx_feed(input_frame_rx_t *rx, const uint8_t *data, size_t len,
                         input_frame_cb_t on_frame, input_text_cb_t on_text, void *user);

#ifdef __cplusplus
}
#endif

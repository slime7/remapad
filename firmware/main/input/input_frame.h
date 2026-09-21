#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 桥接帧协议（PC → 设备）：与 USB-Serial/JTAG 上的 CLI 文本共用一条字节流，靠同步字区分。
 * 帧布局：A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)，CRC-16/CCITT-FALSE
 * 覆盖除末尾两字节外的全部字节。线格式 len 是单字节，解码器按 INPUT_FRAME_WIRE_MAX_PAYLOAD 收帧，
 * 各类型再按自己的上限校验；帧类型表见 ABSTRACTIONS。
 */
#define INPUT_FRAME_SYNC0 0xA5u
#define INPUT_FRAME_SYNC1 0x5Au
#define INPUT_FRAME_VERSION 0x01u
#define INPUT_FRAME_HEADER_LEN 7u
#define INPUT_FRAME_CRC_LEN 2u
/** 输出报告帧载荷上限：DualSense / DualShock 4 蓝牙输出报告各 78 字节
 *  （Report ID + 77 字节字段），比报文帧大一档。 */
#define INPUT_FRAME_OUT_MAX_PAYLOAD 78u
#define INPUT_FRAME_MAX_PAYLOAD INPUT_FRAME_OUT_MAX_PAYLOAD
#define INPUT_FRAME_MAX_LEN \
    (INPUT_FRAME_HEADER_LEN + INPUT_FRAME_MAX_PAYLOAD + INPUT_FRAME_CRC_LEN)
/** 线格式上限：len 字段是单字节，解码器最多收这么多字节的载荷。 */
#define INPUT_FRAME_WIRE_MAX_PAYLOAD 255u
#define INPUT_FRAME_WIRE_MAX_LEN \
    (INPUT_FRAME_HEADER_LEN + INPUT_FRAME_WIRE_MAX_PAYLOAD + INPUT_FRAME_CRC_LEN)

/** 帧类型：输入通路 0x01-0x20（含设备 → PC 的输出报告 0x11 与主机输出
 *  原始采集 0x12），截图 0x21-0x23，OTA 升级 0x30-0x33，amiibo 上传
 *  0x40-0x43，探测 0x7F。 */
typedef enum {
    INPUT_FRAME_TYPE_ATTACH = 0x01,   /**< 载荷 = 设备标识（8 字节）。 */
    INPUT_FRAME_TYPE_DETACH = 0x02,   /**< 载荷 = 设备标识（8 字节）。 */
    INPUT_FRAME_TYPE_REPORT = 0x10,   /**< 载荷 = 设备标识 + 原始报告。 */
    /** 设备 → PC：要写回手柄的输出报告（原始字节，首字节是 Report ID）。 */
    INPUT_FRAME_TYPE_OUT_REPORT = 0x11,
    /** 设备 → PC：主机输出的原始采集（串口 `capture on` 打开）。载荷 =
     *  通道字节 + 标志/长度字节（bit7 截断、低 7 位数据长度）+ 原始字节
     *  （最多 253），帧头 slot 是设备侧记录号，跳号即队列满丢包。 */
    INPUT_FRAME_TYPE_HOST_RAW = 0x12,
    INPUT_FRAME_TYPE_FEEDBACK = 0x20, /**< 载荷 = 反馈（设备 → PC）。 */
    /** 设备 → PC：截图声明。载荷 = 宽 u16 LE + 高 u16 LE + 格式 u8。 */
    INPUT_FRAME_TYPE_IMAGE_INFO = 0x21,
    /** 设备 → PC：截图分块。载荷 = 偏移 u32 LE + 最多 200 字节像素。 */
    INPUT_FRAME_TYPE_IMAGE_DATA = 0x22,
    /** 设备 → PC：截图收尾。载荷 = 总字节数 u32 LE；完整与否由 PC 侧按
     *  偏移是否覆盖满判定，不做跨语言校验算法约定。 */
    INPUT_FRAME_TYPE_IMAGE_END = 0x23,
    INPUT_FRAME_TYPE_OTA_BEGIN = 0x30, /**< 载荷 = 镜像大小声明（设备 ← PC）。 */
    INPUT_FRAME_TYPE_OTA_DATA = 0x31,  /**< 载荷 = 块序号 + 镜像数据（设备 ← PC）。 */
    INPUT_FRAME_TYPE_OTA_END = 0x32,   /**< 无载荷，声明数据发完（设备 ← PC）。 */
    INPUT_FRAME_TYPE_OTA_ACK = 0x33,   /**< 载荷 = 升级状态应答（设备 → PC）。 */
    /** 载荷 = 名称声明（名称长度 + 名称 + 540 字节大小，设备 ← PC）。 */
    INPUT_FRAME_TYPE_AMIIBO_BEGIN = 0x40,
    /** 载荷 = 偏移(u16 LE) + 最多 200 字节镜像数据（设备 ← PC）。 */
    INPUT_FRAME_TYPE_AMIIBO_DATA = 0x41,
    INPUT_FRAME_TYPE_AMIIBO_END = 0x42, /**< 无载荷，声明数据发完（设备 ← PC）。 */
    /** 载荷 = 状态 + 错误码 + 已收字节(u32 LE) + 槽位号（设备 → PC）。 */
    INPUT_FRAME_TYPE_AMIIBO_ACK = 0x43,
    INPUT_FRAME_TYPE_PING = 0x7F,     /**< 载荷 = 版本号（1 字节）。 */
} input_frame_type_t;

/** 截图声明的载荷长度与像素格式；格式 1 = RGB565 小端，行序自上而下。 */
#define INPUT_FRAME_IMAGE_INFO_LEN 5u
#define INPUT_FRAME_IMAGE_FORMAT_RGB565_LE 1u
/** 截图分块的偏移字段长度与单块像素上限（与 OTA 数据帧同量级，避免
 *  设备→PC 方向再多一档缓冲尺寸）。 */
#define INPUT_FRAME_IMAGE_OFF_LEN 4u
#define INPUT_FRAME_IMAGE_CHUNK_MAX 200u
/** IMAGE_END 载荷长度：总字节数 u32。 */
#define INPUT_FRAME_IMAGE_END_LEN 4u

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

/**
 * 线格式编码入口：字节布局与 input_frame_encode 完全一致，但载荷上限放宽到
 * INPUT_FRAME_WIRE_MAX_PAYLOAD（255 字节）。OTA 数据帧（202 字节）与截图
 * 分块帧（偏移 + 200 字节像素）走这里；报文帧继续用 input_frame_encode，
 * 把上限钉在 INPUT_FRAME_MAX_PAYLOAD。输出缓冲不足同样返回 0。
 */
size_t input_frame_encode_wire(uint8_t *out, size_t out_len, uint8_t type, uint8_t slot,
                               uint8_t seq, const uint8_t *payload, size_t payload_len);

/** 解帧状态机：把字节流切成桥接帧与其余文本（CLI 行命令）。 */
typedef struct {
    uint8_t buf[INPUT_FRAME_WIRE_MAX_LEN + 16u];
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

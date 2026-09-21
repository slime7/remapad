#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 主机输出原始采集（诊断通道）：把主机写进输出特征值的原始字节（布局解析与结构化事件之前）
 * 排队交给数据面任务，经桥接帧 HOST_RAW 回传 PC。采集默认关闭、串口 `capture on|off` 控制；
 * 写入侧跑在 NimBLE 主机任务里，本模块只做入环拷贝，串口传输留在数据面任务。
 * 通道字节与名称见 docs/controller-switch2.md 的 GATT 属性表。
 */
#define DP_CAPTURE_CH_BASE_CONFIG 0x05u
#define DP_CAPTURE_CH_RUMBLE 0x12u
#define DP_CAPTURE_CH_CMD 0x14u
#define DP_CAPTURE_CH_COMPOSITE 0x16u
#define DP_CAPTURE_CH_FWUPG 0x18u
#define DP_CAPTURE_CH_EXT22 0x22u
#define DP_CAPTURE_CH_EXT26 0x26u
#define DP_CAPTURE_CH_EXT2A 0x2Au
#define DP_CAPTURE_CH_EXT2C 0x2Cu
#define DP_CAPTURE_CH_EXT2E 0x2Eu
#define DP_CAPTURE_CH_EXT32 0x32u

/** 单条记录的数据上限：桥接帧线格式载荷 255 字节去掉通道与标志两字节。 */
#define DP_CAPTURE_MAX_DATA 253u
/** 排队深度：震动流约 66 Hz、数据面 5 ms 一拍，余量留给初始化期的命令突发。 */
#define DP_CAPTURE_QUEUE_LEN 12u

/** 一条主机输出记录（原始字节原样保存，超长截断并带标记）。 */
typedef struct {
    uint8_t channel;
    uint8_t len;
    bool truncated;
    uint8_t seq; /**< 设备侧记录号（回传给 PC 检测跳号 = 队列满丢包）。 */
    uint8_t data[DP_CAPTURE_MAX_DATA];
} dp_capture_record_t;

/**
 * 开/关采集。开关同时清空队列与计数：每次采集都从干净的现场开始，
 * 关闭前残留的记录不属于下一次采集。
 */
void dp_capture_set_enabled(bool on);

/** 采集是否开启（诊断回显；写入侧的快速判据同源）。 */
bool dp_capture_enabled(void);

/**
 * 记一次主机写入（解析之前调用）：入队失败（队列满）按丢包计数并丢弃
 * 本条，不覆盖已排队的记录。采集关闭时只有一次布尔读的开销。
 */
void dp_capture_host_write(uint8_t channel, const uint8_t *data, size_t len);

/**
 * 取出最早一条记录并直接编码成桥接帧载荷（通道 + 标志/长度 + 数据，
 * 标志字节 bit7 = 截断、低 7 位 = 数据长度），返回载荷长度；没有记录返回 0。
 * cap 不够容纳时记录留在队列里，由调用方给足缓冲。
 */
size_t dp_capture_pop_payload(uint8_t *out, size_t cap, uint8_t *slot);

/** 已入队与已丢弃的记录数（CLI 回显）：丢弃只发生在队列写满时。 */
void dp_capture_counts(uint32_t *pushed, uint32_t *dropped);

#ifdef __cplusplus
}
#endif

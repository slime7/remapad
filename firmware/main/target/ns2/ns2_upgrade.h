#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 主机推送的手柄固件更新：0x0018 上的记录流装配（协议见 docs/controller-switch2.md）。
 *
 * 主机把一条 0x0d/0x04 命令帧切成若干记录逐条写入（记录头 4 字节：类型、序号、载荷长度小端），
 * 载荷按到达顺序拼成 8 字节帧头（体长在字节 4-5，大端）+ 帧体。
 * 本模块只做装配与留样、不解释帧体语义；伪装应答由 ble_session 依据装配结果发出。
 */

/** 帧装配缓冲上限：一帧约 4108 字节，留足余量。 */
#define NS2_UPGRADE_FRAME_CAP 6144u
/** 首帧留样上限：块结构（帧头、块大小、数据起点）只在第一帧里看得清。 */
#define NS2_UPGRADE_SAMPLE_CAP 4224u
/** 记录流留样上限：始终保留最近这么多个字节（含记录头）。 */
#define NS2_UPGRADE_TAIL_CAP 256u
/** 记录头长度。 */
#define NS2_UPGRADE_RECORD_HEADER_LEN 4u
/** 记录头首字节：帧首记录 / 续帧记录。 */
#define NS2_UPGRADE_RECORD_FIRST 0x01u
#define NS2_UPGRADE_RECORD_NEXT 0x02u

/** 一次 feed 的结果。 */
typedef enum {
    NS2_UPGRADE_NONE = 0,  /**< 记录已收下，帧还没凑齐。 */
    NS2_UPGRADE_FRAME,     /**< 一帧凑齐：frame / frame_len 有效，应回应答。 */
    NS2_UPGRADE_MALFORMED, /**< 记录头不完整，已计入统计但不参与装配。 */
} ns2_upgrade_event_t;

typedef struct {
    uint32_t records;     /**< 收到的记录数（含畸形记录）。 */
    uint32_t bytes;       /**< 收到的记录字节数（含记录头）。 */
    uint32_t frames;      /**< 凑齐的帧数。 */
    size_t min_record;    /**< 记录长度下限（无记录时为 SIZE_MAX）。 */
    size_t max_record;    /**< 记录长度上限。 */
    bool truncated;       /**< 某帧体超过缓冲：该帧不再报完成。 */
    bool frame_ready;     /**< 上一帧已交出、尚未被下一条记录让位。 */
    size_t frame_len;     /**< 当前帧已装配字节数。 */
    uint8_t frame[NS2_UPGRADE_FRAME_CAP];
    size_t sample_len;    /**< 首帧留样字节数。 */
    uint8_t sample[NS2_UPGRADE_SAMPLE_CAP];
    size_t tail_len;      /**< 记录流留样字节数。 */
    uint8_t tail[NS2_UPGRADE_TAIL_CAP];
} ns2_upgrade_t;

/** 清空一次升级会话的装配状态（计数与留样一并复位）。 */
void ns2_upgrade_reset(ns2_upgrade_t *up);

/** 喂入一条记录（0x0018 的一次写入）。 */
ns2_upgrade_event_t ns2_upgrade_feed(ns2_upgrade_t *up, const uint8_t *data, size_t len);

/** 帧头声明的体长（字节 4-5，大端）；帧未凑齐 8 字节时返回 0。 */
size_t ns2_upgrade_frame_body(const ns2_upgrade_t *up);

#ifdef __cplusplus
}
#endif

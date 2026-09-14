#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 原始报告长度上限（USB HID 报告实际不超过 64 字节）。 */
#define PAD_REPORT_MAX 64

/**
 * 一帧原始报告与设备标识：PC 桥接（input/ 分帧解码）与将来的 USB host
 * 直插共用同一种载荷，解析与映射只在固件做一份。
 */
typedef struct {
    /** 家族提示：PC 侧按 VID/PID 判定后随帧传来；未知填 PAD_FAMILY_UNKNOWN，
     *  解析侧会按 VID/PID 再判一次，仍不认识则按 Xbox 布局兜底。 */
    pad_family_t family;
    pad_conn_t conn;
    uint16_t vid;
    uint16_t pid;
    uint8_t report_id;
    uint8_t len;
    uint32_t seq;
    uint8_t data[PAD_REPORT_MAX];
} pad_report_t;

/** 按 VID/PID 判定家族；不认识返回 PAD_FAMILY_UNKNOWN。 */
pad_family_t pad_family_from_ids(uint16_t vid, uint16_t pid);

/**
 * 把一帧原始报告解析成私有格式：按键按家族表映射到位置语义、摇杆与扳机
 * 归一到 0-4095、套用死区、方向键帽子开关展开，能力位标注这一帧里哪些
 * 字段真的来自设备。
 */
void pad_state_from_report(const pad_report_t *report, pad_state_t *state);

#ifdef __cplusplus
}
#endif

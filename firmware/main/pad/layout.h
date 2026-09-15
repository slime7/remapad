#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pad_device.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 字段偏移缺省值：该布局没有这个字段。 */
#define PAD_OFF_NONE 0xFF

/** 摇杆原始格式。 */
typedef enum {
    PAD_STICK_U8 = 0, /**< 单字节，中心 0x80（PS、Steam 原生报告）。 */
    PAD_STICK_I16,    /**< 有符号 16 位小端，中心 0（Xbox）。 */
} pad_stick_style_t;

/**
 * 家族布局表的一行：按（家族, Report ID, 连接方式, PID）定位字段偏移。偏移
 * 一律从收到的报告首字节起算（含 Report ID）。
 *
 * 同一组合下有多个型号时报 PID 分行（PS 系三种型号都报 0x01，但字段偏移各不
 * 相同）；布局相同的多个 PID 写在同一行的 pids 里；pids 为空表示该组合下所有
 * 型号共用这行。报告未带 PID（离线构造或旧帧）时不做型号过滤，取最先匹配的行。
 *
 * 各系列的行放在 pad/layouts/ 下，一族一个文件；加一个系列＝加一个文件并在
 * layout.c 的模块表里登记一行。偏移初值多取自公开资料，实机接线时用
 * `pc/bridge.py --dump` 抓包核对，偏差只影响布局文件，不影响上下游。
 */
typedef struct {
    pad_family_t family;
    /** 连接方式；PAD_CONN_UNKNOWN 表示有线与蓝牙共用这一行。 */
    pad_conn_t conn;
    uint8_t report_id;
    /** 该行适用的 PID，0 结尾；首元素为 0 表示不按型号过滤。 */
    uint16_t pids[4];
    uint8_t buttons_off;
    uint8_t buttons_bytes;
    /** 方向键帽子开关偏移；0xFF 表示方向键在按键位图里。 */
    uint8_t hat_off;
    uint8_t trigger_off[PAD_TRIGGER_COUNT];
    uint8_t stick_off[PAD_AXIS_COUNT];
    uint8_t touch_off;
    uint8_t motion_off;
    uint8_t battery_off;
    uint16_t touch_max_x;
    uint16_t touch_max_y;
    pad_stick_style_t stick_style;
    uint32_t caps;
    /** 设备 Y 轴向下为正时置位，解析侧翻成「上为正」。 */
    bool invert_y;
    const uint32_t *btn_map;
} pad_layout_t;

/** 布局模块：一个手柄系列的全部布局行（一族一个文件，见 pad/layouts/）。 */
typedef struct {
    const char *name;
    const pad_layout_t *rows;
    size_t row_count;
} pad_layout_module_t;

/** Xbox 家族的按键位（bit0-3 方向键、bit4 Menu、bit5 View、bit6/7 摇杆按下、
 *  bit8/9 肩键、bit10 西瓜键、bit11 分享键、bit12-15 面键）。未识别型号的兜底
 *  布局也用它，因此定义在 layout.c 与 layouts/xbox.c 共用。 */
extern const uint32_t pad_xbox_btn_map[16];

/** PS 家族（DS4 与 DualSense）共用的按键位序：方向键帽子开关在低四位、面键在
 *  高四位，肩键、Create/Options、摇杆按下、PS / 触摸板 / 静音与 Edge 背键依次
 *  排在第二、三字节。DS3 的方向键在按键位图里，自带一份。 */
extern const uint32_t pad_ps_btn_map[24];

/**
 * 按报告的设备标识找布局行：家族取报告里的值，为空时按 VID/PID 判定；家族、
 * Report ID、连接方式、PID 任一不符就换下一行。找不到返回 NULL，由调用方决定
 * 是否套用兜底布局。family 输出实际使用的家族（可能由 VID/PID 补出）。
 */
const pad_layout_t *pad_layout_find(const pad_report_t *report, pad_family_t *family);

/** 未识别型号的兜底布局：按 Xbox 有线解析，能力位由调用方标记。 */
const pad_layout_t *pad_layout_fallback(void);

#ifdef __cplusplus
}
#endif


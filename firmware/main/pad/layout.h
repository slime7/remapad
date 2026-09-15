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
    PAD_STICK_U12,    /**< 12 位紧凑打包三字节（NS2 的 0x05 / 0x09 报文体）。 */
} pad_stick_style_t;

/** 电量字节风格。 */
typedef enum {
    PAD_BATTERY_PS = 0, /**< 低四位是 0-10 档、bit4 表示充电中（DS3 / DS4 / DualSense）。 */
    PAD_BATTERY_NS2,    /**< bit0 外部供电、bit1 充电中、bits2-5 电量等级 0-9。 */
} pad_battery_style_t;

/** 玩家灯映射方式。 */
typedef enum {
    PAD_LED_NONE = 0,    /**< 设备没有可控灯。 */
    PAD_LED_PLAYER_MASK, /**< 主机玩家灯掩码写进候选字节。 */
    PAD_LED_LIGHTBAR,    /**< 灯条：掩码换算成一组颜色写进 RGB 三字节。 */
} pad_led_style_t;

/** 触觉采样（NS2 的 0x0A 采样回放）在目标设备上的处理方式。 */
typedef enum {
    PAD_HAPTIC_IGNORE = 0,  /**< 没有等价能力，只记日志。 */
    PAD_HAPTIC_AS_RUMBLE,   /**< 退化成一次短震动。 */
    PAD_HAPTIC_VERBATIM,    /**< 设备自己能播采样（NS2 手柄透传，参数原样写回）。 */
} pad_haptic_style_t;

/** 输出报告的收尾方式：字段写完之后的补字节动作。 */
typedef enum {
    PAD_OUT_FRAME_NONE = 0, /**< 写完即可发送（有线形态）。 */
    /** PS 蓝牙形态：末 4 字节是 CRC32（种子字节 0xA2 参与计算，小端），
     *  缺它时主机应声不认——手柄收下报告但一个动作都不做。 */
    PAD_OUT_FRAME_PS_BT,
} pad_out_frame_t;

/**
 * 运动字段描述：一次性给出取样位置、样本数与轴映射。轴映射把来源轴归一到
 * 私有约定（X 右为正、Y 上为正、Z 朝屏幕外为正）：gyro_src / accel_src 的
 * 第 i 项是私有三轴第 i 路取来源的第几路（PAD_OFF_NONE 表示该路缺失），
 * 三项全零表示恒等映射。invert_mask 的 bit0-2 表示陀螺 X/Y/Z 取反、
 * bit3-5 表示加速 X/Y/Z 取反。
 */
typedef struct {
    uint8_t samples; /**< 一次报告里的样本数；0 按 1 处理。 */
    uint8_t stride;  /**< 相邻样本的字节步长；0 按 12 处理。 */
    uint8_t gyro_src[3];
    uint8_t accel_src[3];
    uint8_t invert_mask;
} pad_motion_layout_t;

/**
 * 输出（反馈）报告描述：把主机下发的震动 / 玩家灯 / 触觉采样编码成该设备
 * 能吃的输出报告。presets 是发送前写入的常量字节（偏移 + 值，偏移
 * PAD_OFF_NONE 表示结束），用来点亮 DS4 的 flags 或 DualSense 的两个
 * valid_flag。震动的两路强度按 rumble_max 缩放后写进 rumble_off；玩家灯按
 * led_style 写掩码或 RGB。report_id 为 0 表示该设备没有可写的反馈通道。
 */
#define PAD_OUT_PRESET_MAX 8

typedef struct {
    uint8_t report_id;
    uint8_t len; /**< 输出报告总长度（含 Report ID 字节）。 */
    uint8_t presets[PAD_OUT_PRESET_MAX][2];
    uint8_t rumble_off[PAD_TRIGGER_COUNT];
    uint8_t rumble_max[PAD_TRIGGER_COUNT];
    uint8_t led_mask_off;
    uint8_t led_rgb_off;
    uint8_t led_style; /**< pad_led_style_t。 */
    uint8_t haptic;    /**< pad_haptic_style_t。 */
    uint8_t frame;     /**< pad_out_frame_t。 */
    /** 玩家灯落地值：四项依次对应主机掩码 bit0-3（1P-4P），0 表示原样写主机
     *  掩码。DualSense 的五颗灯是一组固定模式（1P 中灯、2P 中加外），不能直写
     *  主机掩码，需要这张表。 */
    uint8_t led_mask_map[4];
} pad_output_layout_t;

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
    /** 电量字节风格；PAD_CAP_BATTERY 未置位时不参与解析。 */
    pad_battery_style_t battery_style;
    uint32_t caps;
    /** 设备 Y 轴向下为正时置位，解析侧翻成「上为正」。 */
    bool invert_y;
    const uint32_t *btn_map;
    /** 运动字段描述；motion_off 为 PAD_OFF_NONE 表示该型号没有运动数据。 */
    pad_motion_layout_t motion;
    /** 输出（反馈）报告描述；report_id 为 0 表示没有可写的反馈通道。 */
    pad_output_layout_t out;
    /** 设备自带报告语言（pad_lang_t）与期望的目标身份（pad_identity_t）。 */
    uint8_t native_lang;
    uint8_t native_identity;
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

/**
 * 反馈方向查表：主机反馈到达时手上只有设备标识（没有报告帧），这里按
 * VID/PID 与连接方式找布局行，再用行的 out 描述编码输出报告。report_id
 * 不参与匹配（反馈不依赖输入报告格式）。family 输出判定出的家族。
 */
const pad_layout_t *pad_layout_find_by_ids(uint16_t vid, uint16_t pid, pad_conn_t conn,
                                           pad_family_t *family);

/** 未识别型号的兜底布局：按 Xbox 有线解析，能力位由调用方标记。 */
const pad_layout_t *pad_layout_fallback(void);

#ifdef __cplusplus
}
#endif

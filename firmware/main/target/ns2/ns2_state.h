#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 规范化手柄状态：数据面统一按键、摇杆与电源信息后的领域模型，
 * 与线格式解耦；Report 0x05 / 0x09 编码器各自负责按键位图映射。
 */

/** 按键位定义（与线格式无关的稳定枚举）。 */
enum {
    NS2_BTN_A = 1u << 0,
    NS2_BTN_B = 1u << 1,
    NS2_BTN_X = 1u << 2,
    NS2_BTN_Y = 1u << 3,
    NS2_BTN_PLUS = 1u << 4,
    NS2_BTN_MINUS = 1u << 5,
    NS2_BTN_L = 1u << 6,
    NS2_BTN_R = 1u << 7,
    NS2_BTN_ZL = 1u << 8,
    NS2_BTN_ZR = 1u << 9,
    NS2_BTN_LSTICK = 1u << 10,
    NS2_BTN_RSTICK = 1u << 11,
    NS2_BTN_DPAD_UP = 1u << 12,
    NS2_BTN_DPAD_DOWN = 1u << 13,
    NS2_BTN_DPAD_LEFT = 1u << 14,
    NS2_BTN_DPAD_RIGHT = 1u << 15,
    NS2_BTN_HOME = 1u << 16,
    NS2_BTN_CAPTURE = 1u << 17,
    NS2_BTN_C = 1u << 18,
    NS2_BTN_GL = 1u << 19,
    NS2_BTN_GR = 1u << 20,
};

#define NS2_STICK_CENTER 2048
#define NS2_STICK_MAX 4095

/** 0x09 报文的耳机音频状态字节（偏移 0x0D）取值：未插入 / 纯耳机 / 带麦。
 *  另有 0x0D / 0x0F 一档（未确认语义），用串口
 *  `headset 0x0d` 之类的覆盖值。 */
#define NS2_HEADSET_NONE 0x00
#define NS2_HEADSET_STEREO 0x05
#define NS2_HEADSET_WITH_MIC 0x07
/** Report 0x05 按键位图第 3 字节的耳机插入位（插入时置位）。 */
#define NS2_05_BTN3_HEADSET 0x10

/** 0x09 运动块填充方式。真机在特性位 bit2（IMU）开启后发 40 字节传感器
 *  数据；板卡没有 IMU，只能用占位。CLI `motion` 可在几种占位间切换，
 *  确认主机是否校验运动块内容。 */
typedef enum {
    NS2_MOTION_ZERO = 0,  /**< 长度 0x28 + 全零块（默认） */
    NS2_MOTION_CAPTURE = 1, /**< 长度 0x28 + 样本块（时间戳按节奏推进） */
    NS2_MOTION_NONE = 2,  /**< 长度 0x00，不带运动数据 */
    /** 长度 0x28 + 输入设备的真实样本：按 NS1 的三份 12 字节样本风格排布。
     *  0x09 运动块的内部结构没有公开资料，这一档只用于对照与后续
     *  解码，默认不启用（串口 CLI 的 motion 3 打开）。 */
    NS2_MOTION_SENSOR = 3,
} ns2_motion_mode_t;

/**
 * 手柄身份：本设备只模拟 Pro Controller 2。一台控制器只有一个 public 地址，主机也只接受
 * public 地址的广播，左右两只 Joy-Con 无法各自寻址。枚举因此只有一个取值；
 * 凭证表与出厂块仍按身份分槽，将来真要再加型号时不用改存储与广播层的分槽方式。
 */
typedef enum {
    NS2_ID_PRO = 0,
    NS2_ID_COUNT = 1,
} ns2_identity_t;

typedef struct {
    uint32_t buttons;
    uint16_t stick_lx;
    uint16_t stick_ly;
    uint16_t stick_rx;
    uint16_t stick_ry;
    /** 电量等级 0-9，对应 Report 0x09 电源状态 bits2-5。 */
    uint8_t battery_level;
    bool external_power;
    bool charging;
    bool fully_charged;
    /** 电池电压毫伏，Report 0x05 专用字段。 */
    uint16_t battery_mv;
    /** 特性掩码 bit5（触觉震动）开启标志，影响 0x09 状态标志字节。 */
    bool rumble_enabled;
    /** NFC 状态字节（Report 0x09 偏移 0x0C）：0x00 空闲，0x01-0x07 感应中。
     *  由 amiibo 预置数据驱动（ns2_output），无预置时保持 0x00。 */
    uint8_t nfc_state;
    /** 耳机音频状态字节（Report 0x09 偏移 0x0D，NS2_HEADSET_*）：由
     *  ns2_output 按输入设备的 3.5mm 状态填，串口 headset 可现场覆盖；
     *  非 0x00 时 Report 0x05 的耳机插入位一并置位。 */
    uint8_t headset_state;
    /** 运动块填充方式（ns2_motion_mode_t）。 */
    uint8_t motion_mode;
    /** 运动数据：输入设备带 IMU（PAD_CAP_MOTION）时才有效。板卡本身没有
     *  IMU，0x05 的 IMU 字段与 0x09 的实验运动块都取自这里。 */
    bool motion_valid;
    int16_t gyro[3];
    int16_t accel[3];
} ns2_controller_state_t;

/** 复位为静置默认：摇杆居中、无按键、无外设数据。 */
static inline void ns2_state_defaults(ns2_controller_state_t *state)
{
    state->buttons = 0;
    state->stick_lx = NS2_STICK_CENTER;
    state->stick_ly = NS2_STICK_CENTER;
    state->stick_rx = NS2_STICK_CENTER;
    state->stick_ry = NS2_STICK_CENTER;
    state->battery_level = 0;
    state->external_power = false;
    state->charging = false;
    state->fully_charged = false;
    state->battery_mv = 0;
    state->rumble_enabled = false;
    state->nfc_state = 0;
    state->headset_state = NS2_HEADSET_NONE;
    state->motion_mode = NS2_MOTION_ZERO;
    state->motion_valid = false;
    state->gyro[0] = 0;
    state->gyro[1] = 0;
    state->gyro[2] = 0;
    state->accel[0] = 0;
    state->accel[1] = 0;
    state->accel[2] = 0;
}

#ifdef __cplusplus
}
#endif

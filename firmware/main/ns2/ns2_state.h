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
}

#ifdef __cplusplus
}
#endif

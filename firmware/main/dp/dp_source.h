#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 数据面输入源抽象：把「输入从哪来」与「编码成什么目标报文」解耦，新增输入设备
 * （USB host 手柄、桥接 PC 报告、调试注入）只需实现一个 dp_source_t 并注册。
 * 合成规则：第一个已注册源拥有摇杆、扳机、触摸、运动与设备标识字段，后续源只叠加按键，
 * 调试注入最后叠加。
 */

typedef struct {
    /** 源名称（日志用）。 */
    const char *name;
    /** 采样：在 pad_state_defaults 之后的私有状态上填入本源数据。 */
    void (*sample)(pad_state_t *state);
} dp_source_t;

/** 注册输入源（静态生命周期，注册后不可注销）。先注册者优先拥有摇杆。 */
void dp_source_register(const dp_source_t *source);

/** 合成所有已注册源 + 调试注入，输出当前周期的私有手柄状态。 */
void dp_source_sample(pad_state_t *state);

/** 当前数据面节拍（毫秒）：注入的保持时长按它换算成拍数，节拍切换后
 *  （BLE 关闭时省电档 83 ms）一次注入的按住时长不走样。 */
void dp_source_set_tick_ms(uint32_t tick_ms);

/** 调试注入：叠加一次按键按下，保持 hold_ms 后自动释放。 */
void dp_source_inject(uint32_t buttons_mask, uint32_t hold_ms);

/** 调试注入：立即释放当前注入的按键（保持期未到也清零）。 */
void dp_source_inject_release(void);

/** 调试注入：设定一侧摇杆电平（side 取 'l' / 'r'，x/y 取 0-4095，超界
 *  钳制到边界）。设定后持续保持，直到再次设定或 dp_source_inject_stick_reset；
 *  两侧独立，未设定的一侧沿用输入源给出的摇杆值。 */
void dp_source_inject_stick(char side, uint16_t x, uint16_t y);

/** 调试注入：两侧摇杆回中并解除摇杆注入（之后输入源的摇杆值恢复生效）。 */
void dp_source_inject_stick_reset(void);

/** 调试按键名（a / home / ui / up / ls / …）→ 私有按键位与默认保持时长：
 *  命中返回 true，未命中返回 false。名字表在 dp_source.c，CLI 与用例共用。 */
bool dp_source_key_lookup(const char *name, size_t len, uint32_t *mask, uint32_t *hold_ms);

/** 最近的注入按键是否仍在保持期（诊断用）。 */
bool dp_source_inject_active(void);

#ifdef __cplusplus
}
#endif

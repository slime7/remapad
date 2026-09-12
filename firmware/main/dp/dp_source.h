#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 数据面输入源抽象（ADR 0011 的 dp 模块内）：把「输入从哪来」与「NS2 编码
 * 输出」解耦。dp_task 每个周期调用 dp_source_sample 得到合成后的规范化状态，
 * 再交给 ns2_output_send；新增输入设备（USB host 手柄、UART 注入、桥接 PC
 * 报告等）只需实现一个 dp_source_t 并注册，不改编码与发送路径。
 *
 * 合成规则：第一个已注册源拥有摇杆与电源字段（通常即主输入设备），后续源
 * 只叠加按键；最后叠加调试注入（overlay）。
 */

typedef struct {
    /** 源名称（日志用）。 */
    const char *name;
    /** 采样：在 ns2_state_defaults 之后的规范化状态上填入本源数据。 */
    void (*sample)(ns2_controller_state_t *state);
} dp_source_t;

/** 注册输入源（静态生命周期，注册后不可注销）。先注册者优先拥有摇杆。 */
void dp_source_register(const dp_source_t *source);

/** 合成所有已注册源 + 调试注入，输出当前周期的规范化状态。 */
void dp_source_sample(ns2_controller_state_t *state);

/** 调试注入：叠加一次按键按下，保持 hold_ms 后自动释放。 */
void dp_source_inject(uint32_t buttons_mask, uint32_t hold_ms);

/** 最近的注入按键是否仍在保持期（诊断用）。 */
bool dp_source_inject_active(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 数据面节拍与省电档：正常档 5 ms 采样（每 15 ms 一份报告），BLE 栈关闭
 * （未连接也未广播）时切到省电档 83 ms（12 fps 等效）——此时没有链路可上报，
 * 采样慢下来只影响屏幕刷新与调试注入的响应速度。
 * 判据与换算都是纯逻辑（主机端用例见 firmware/test/test_dp_power.c），
 * 数据面任务与状态快照装配共用同一份。
 */

/** 正常档采样间隔（毫秒）。 */
#define DP_TICK_MS 5u

/** 省电档采样间隔（毫秒）：约 12 fps 等效。 */
#define DP_POWER_SAVE_TICK_MS 83u

/** 省电档判据：BLE 栈没在跑（未连接也未广播）就是省电档。 */
bool dp_power_save_active(bool ble_stack_running);

/** 采样间隔随档位切换（毫秒）。 */
uint32_t dp_tick_ms(bool power_save);

/** 每几拍发一份报告：节拍长于上报间隔时按每拍一份走，永远不为零。 */
uint32_t dp_report_divisor(uint32_t tick_ms, uint32_t report_interval_ms);

#ifdef __cplusplus
}
#endif

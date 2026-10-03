#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 输入链路读数：输入报文到达率与输入数据发出延迟的轻量统计，给底栏速率格
 * 与串口诊断看。到达打点在输入源收包处（USB 直插与桥接两个入口），发出打点
 * 在数据面的上报路径；读数由界面状态轮询拉取。只记当值，不留历史。
 */

/** 输入报文到达打点（输入源任务上下文，now_us 取 esp_timer_get_time()）。 */
void dp_stats_note_input_us(int64_t now_us);

/** 输入报告发出打点（数据面任务上下文）：对最新到达打点求差并做 1/8 EMA，
 *  输入数据已停发（差值为负或超过 1 秒）时不记。 */
void dp_stats_note_output_us(int64_t now_us);

/** 输入报文到达率（Hz）：1 秒窗差分，窗口未满沿用上一窗读数，上限 9999。 */
int dp_stats_input_hz(void);

/** 同 dp_stats_input_hz，时钟由调用方给（主机端用例注入确定时钟）。 */
int dp_stats_input_hz_at(int64_t now_us);

/** 输入数据发出延迟（毫秒，EMA 取整；0 = 还没有读数）。 */
int dp_stats_latency_ms(void);

/** 清零全部读数：主机端用例的确定性入口。 */
void dp_stats_reset(void);

#ifdef __cplusplus
}
#endif

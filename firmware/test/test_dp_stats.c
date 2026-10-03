/**
 * 输入链路读数（dp_stats.c）主机端用例：速率按 1 秒窗差分并封顶，延迟对
 * 「到达 → 发出」差值做 EMA，输入停发后不产生新读数。
 */
#include "host_test.h"

#include <stdint.h>

#include "dp_stats.h"

static void rate_counts_per_second_window(void)
{
  dp_stats_reset();
  /* 首次读数锚定窗口基线，窗口未满沿用上一窗（0）。 */
  CHECK_EQ(dp_stats_input_hz_at(0), 0);
  dp_stats_note_input_us(100000);
  dp_stats_note_input_us(200000);
  CHECK_EQ(dp_stats_input_hz_at(500000), 0);
  CHECK_EQ(dp_stats_input_hz_at(1000000), 2);
  dp_stats_note_input_us(1500000);
  CHECK_EQ(dp_stats_input_hz_at(2000000), 1);
}

static void rate_caps_and_drops_to_zero_when_idle(void)
{
  dp_stats_reset();
  CHECK_EQ(dp_stats_input_hz_at(0), 0);
  for (int i = 0; i < 20000; ++i) {
    dp_stats_note_input_us(500000);
  }
  CHECK_EQ(dp_stats_input_hz_at(1000000), 9999);
  /* 输入停发：下一个整窗差分为 0，读数跟着归零。 */
  CHECK_EQ(dp_stats_input_hz_at(2000000), 0);
}

static void latency_averages_arrival_to_output_gap(void)
{
  dp_stats_reset();
  /* 没有到达打点就没有延迟读数。 */
  dp_stats_note_output_us(1000000);
  CHECK_EQ(dp_stats_latency_ms(), 0);
  /* 首个差值直接进 EMA，之后按 1/8 收敛：10ms 再遇 20ms → 11.25ms。 */
  dp_stats_note_input_us(1000000);
  dp_stats_note_output_us(1010000);
  CHECK_EQ(dp_stats_latency_ms(), 10);
  dp_stats_note_output_us(1020000);
  CHECK_EQ(dp_stats_latency_ms(), 11);
  /* 输入已停发（差值超过 1 秒）：不产生新读数。 */
  dp_stats_note_output_us(3000000);
  CHECK_EQ(dp_stats_latency_ms(), 11);
}

static void reset_clears_everything(void)
{
  dp_stats_reset();
  dp_stats_note_input_us(1000000);
  dp_stats_note_output_us(1010000);
  dp_stats_reset();
  CHECK_EQ(dp_stats_input_hz_at(2000000), 0);
  CHECK_EQ(dp_stats_latency_ms(), 0);
}

HOST_TEST_SUITE(suite_dp_stats, "dp_stats", { "速率按一秒窗差分", rate_counts_per_second_window },
                { "速率封顶且停流归零", rate_caps_and_drops_to_zero_when_idle },
                { "延迟做 EMA 且停发不记", latency_averages_arrival_to_output_gap },
                { "复位清零全部读数", reset_clears_everything });

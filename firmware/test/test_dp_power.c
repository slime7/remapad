/**
 * 数据面节拍与省电档（dp_power.c）主机端用例：BLE 关闭即省电档，
 * 节拍与上报分频的换算在省电档下仍然自洽（分频永远不为零）。
 */
#include "host_test.h"

#include "dp_power.h"

static void power_save_follows_ble_stack(void)
{
  /* BLE 栈没在跑（既没连接也没广播）就是省电档。 */
  CHECK(dp_power_save_active(false));
  CHECK(!dp_power_save_active(true));
}

static void tick_follows_power_save(void)
{
  CHECK_EQ(dp_tick_ms(false), DP_TICK_MS);
  CHECK_EQ(dp_tick_ms(true), DP_POWER_SAVE_TICK_MS);
  /* 省电档是 12 fps 等效节拍：1000 ms / 12 ≈ 83 ms。 */
  CHECK_EQ(DP_POWER_SAVE_TICK_MS, 83u);
}

static void report_divisor_never_zero(void)
{
  /* 正常档：5 ms 采样、每 15 ms 一份报告。 */
  CHECK_EQ(dp_report_divisor(DP_TICK_MS, 15u), 3u);
  /* 省电档：节拍长于上报间隔，按每拍一份报告走——分频为 0 会永远不发报告。 */
  CHECK_EQ(dp_report_divisor(DP_POWER_SAVE_TICK_MS, 15u), 1u);
  CHECK_EQ(dp_report_divisor(15u, 15u), 1u);
  /* 节拍为 0 时按不分频处理，不做除法。 */
  CHECK_EQ(dp_report_divisor(0u, 15u), 1u);
}

HOST_TEST_SUITE(suite_dp_power, "dp_power", { "BLE 关闭即省电档", power_save_follows_ble_stack },
                { "节拍随省电档切换", tick_follows_power_save }, { "上报分频永远不为零", report_divisor_never_zero });

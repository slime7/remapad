#include "battery_curve.h"

/* 电池电压到电量的换算（与硬件无关，主机端用例直接编译本文件，ADC 采样留在 battery.c）：
 * 按单节锂离子的静置电压—容量对照表分段线性插值，并重锚到板载 ADC 的 2870 - 4070 mV 工作范围；
 * 高于 4070 mV 钳位 100%、低于 2870 mV 钳位 0%。 */
typedef struct {
  uint16_t voltage_mv;
  uint8_t percentage;
} battery_curve_point_t;

static const battery_curve_point_t s_curve[] = {
  { 4070, 100 }, { 4005, 95 }, { 3954, 90 }, { 3915, 85 }, { 3838, 80 }, { 3786, 75 }, { 3747, 70 },
  { 3696, 65 },  { 3644, 60 }, { 3618, 55 }, { 3605, 50 }, { 3580, 45 }, { 3554, 40 }, { 3541, 35 },
  { 3515, 30 },  { 3489, 25 }, { 3464, 20 }, { 3438, 15 }, { 3412, 10 }, { 3309, 5 },  { 2870, 0 },
};

uint8_t battery_percent_from_mv(uint32_t voltage_mv)
{
  const uint32_t count = sizeof(s_curve) / sizeof(s_curve[0]);
  if (voltage_mv >= s_curve[0].voltage_mv) {
    return s_curve[0].percentage;
  }
  if (voltage_mv <= s_curve[count - 1].voltage_mv) {
    return s_curve[count - 1].percentage;
  }
  for (uint32_t index = 1; index < count; index++) {
    if (voltage_mv < s_curve[index].voltage_mv) {
      continue;
    }
    const uint32_t span_mv = s_curve[index - 1].voltage_mv - s_curve[index].voltage_mv;
    const uint32_t into_mv = voltage_mv - s_curve[index].voltage_mv;
    const uint32_t span_pct = s_curve[index - 1].percentage - s_curve[index].percentage;
    return (uint8_t)(s_curve[index].percentage + (span_pct * into_mv + span_mv / 2) / span_mv);
  }
  return s_curve[count - 1].percentage;
}

uint8_t battery_ns2_level_from_percent(uint8_t percentage)
{
  const uint8_t clamped = percentage > 100 ? 100 : percentage;
  const uint8_t level = (uint8_t)(clamped / 10);
  return level > 9 ? 9 : level;
}

uint32_t battery_mv_from_percent(uint8_t percentage)
{
  const uint32_t count = sizeof(s_curve) / sizeof(s_curve[0]);
  const uint32_t target = percentage > 100 ? 100u : percentage;
  if (target >= s_curve[0].percentage) {
    return s_curve[0].voltage_mv;
  }
  if (target <= s_curve[count - 1].percentage) {
    return s_curve[count - 1].voltage_mv;
  }
  for (uint32_t index = 1; index < count; index++) {
    if (target < s_curve[index].percentage) {
      continue;
    }
    /* target 落在 index-1（电量更高）与 index（电量更低）两档之间。 */
    const uint32_t span_pct = s_curve[index - 1].percentage - s_curve[index].percentage;
    const uint32_t span_mv = s_curve[index - 1].voltage_mv - s_curve[index].voltage_mv;
    const uint32_t into_pct = target - s_curve[index].percentage;
    return s_curve[index].voltage_mv + (span_mv * into_pct + span_pct / 2) / span_pct;
  }
  return s_curve[count - 1].voltage_mv;
}

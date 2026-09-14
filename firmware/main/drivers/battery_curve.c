#include "battery_curve.h"

/* 电池电压到电量的换算与硬件无关，因此单独成文件：主机端用例直接编译
 * 本文件（见 firmware/test/test_battery.c），ADC 采样留在 battery.c。
 *
 * 单节锂离子电池没有线性电量表：3.7-3.9 V 一段电压对应很大一块容量，
 * 4.0 V 以上一段电压只对应几个百分点。下面用行业常用的静置电压—容量
 * 对照表做分段线性插值，比「空/中/满」三点线性更接近真实剩余电量。
 * 表值是静置电压；设备带屏和 BLE 负载时端电压会低几十毫伏，读数据此
 * 略微偏保守。 */
typedef struct {
    uint16_t voltage_mv;
    uint8_t percentage;
} battery_curve_point_t;

static const battery_curve_point_t s_curve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75},
    {3950, 70},  {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45},
    {3800, 40},  {3790, 35}, {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15},
    {3690, 10},  {3610, 5},  {3270, 0},
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
        return (uint8_t)(s_curve[index].percentage +
                         (span_pct * into_mv + span_mv / 2) / span_mv);
    }
    return s_curve[count - 1].percentage;
}

uint8_t battery_ns2_level_from_percent(uint8_t percentage)
{
    const uint8_t clamped = percentage > 100 ? 100 : percentage;
    const uint8_t level = (uint8_t)(clamped / 10);
    return level > 9 ? 9 : level;
}

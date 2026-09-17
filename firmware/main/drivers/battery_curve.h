#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 电池电压（毫伏）折算剩余电量百分比（0-100）。 */
uint8_t battery_percent_from_mv(uint32_t voltage_mv);

/** 电量百分比折算名义端电压（毫伏）：电压—容量表的反演。输入设备只报档位
 *  不带电压，上发主机的 0x05 报文电池电压字段用它折算（同表来回折算误差
 *  在一个百分点内）。 */
uint32_t battery_mv_from_percent(uint8_t percentage);

/** 电量百分比折算 NS2 报告 0x09 电源状态的电量等级（0-9，9 为满）。 */
uint8_t battery_ns2_level_from_percent(uint8_t percentage);

#ifdef __cplusplus
}
#endif

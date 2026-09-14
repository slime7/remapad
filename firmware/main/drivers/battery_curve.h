#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 电池电压（毫伏）折算剩余电量百分比（0-100）。 */
uint8_t battery_percent_from_mv(uint32_t voltage_mv);

/** 电量百分比折算 NS2 报告 0x09 电源状态的电量等级（0-9，9 为满）。 */
uint8_t battery_ns2_level_from_percent(uint8_t percentage);

#ifdef __cplusplus
}
#endif

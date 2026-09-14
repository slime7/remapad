#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化电池 ADC（BAT_ADC=GPIO1，B+ 经 200K/100K 分压）并启动采样任务。 */
esp_err_t battery_init(void);

/** 读取电池电压（毫伏，已按分压比还原为 VBAT）。 */
uint32_t battery_get_voltage_mv(void);

/** 读取电量估算（0 - 100，由电压按静置电压—容量表折算）。 */
uint8_t battery_get_percentage(void);

/** 读取充电状态：板载 ETA6098 的状态脚没有引到 GPIO，无法直接测量，
 *  该值由采样窗口内的电压趋势推断（见 battery.c）。 */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif

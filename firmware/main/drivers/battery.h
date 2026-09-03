#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 预留电池 ADC 驱动初始化接口。 */
esp_err_t battery_init(void);

/** 读取预留的电池电压接口（毫伏）。 */
uint32_t battery_get_voltage_mv(void);

/** 读取预留的电量估算接口（0 - 100）。 */
uint8_t battery_get_percentage(void);

/** 读取预留的充电状态接口。 */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif

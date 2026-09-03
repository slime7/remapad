#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化电池采样 ADC 驱动
 */
esp_err_t battery_init(void);

/**
 * 获取当前电池采样电压 (毫伏)
 */
uint32_t battery_get_voltage_mv(void);

/**
 * 获取当前估算电量百分比 (0 - 100)
 */
uint8_t battery_get_percentage(void);

/**
 * 检查是否处于充电状态
 */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif

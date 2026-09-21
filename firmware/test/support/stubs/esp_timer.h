#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 主机端桩：单调时钟（微秒）。本硬件上由 ESP-IDF 提供；主机端以每次调用
 *  +1000us 的确定步进模拟，用例按调用次数推进时间。 */
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif

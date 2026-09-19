#include "esp_timer.h"

/** 主机端确定时钟：每调用一次推进 1ms，保证用例可复现。 */
int64_t esp_timer_get_time(void)
{
    static int64_t now_us;
    now_us += 1000;
    return now_us;
}

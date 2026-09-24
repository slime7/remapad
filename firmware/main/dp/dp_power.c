/** 数据面节拍与省电档换算：判据与分频都不碰硬件，主机端用例见
 *  firmware/test/test_dp_power.c。 */
#include "dp_power.h"

bool dp_power_save_active(bool ble_stack_running)
{
    return !ble_stack_running;
}

uint32_t dp_tick_ms(bool power_save)
{
    return power_save ? DP_POWER_SAVE_TICK_MS : DP_TICK_MS;
}

uint32_t dp_report_divisor(uint32_t tick_ms, uint32_t report_interval_ms)
{
    if (tick_ms == 0u) {
        return 1u;
    }
    const uint32_t divisor = report_interval_ms / tick_ms;
    return divisor > 0u ? divisor : 1u;
}

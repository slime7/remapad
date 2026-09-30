#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 无线调试会话的断线重连节奏：失败初期每 3 秒重试一档（AP 重启之类的瞬时故障
 * 很快能回来），第 5 次失败起拉长到 30 秒（连不上的持续扫射只剩发热），累计
 * 第 10 次失败不再重连——隔一小拍就整段关闭会话（射频断电），要再连只能手动开。
 * 纯逻辑（主机端用例见 firmware/test/test_netlog_retry.c）；失败计数与会话
 * 生命周期的接线在 netlog.c。
 */

/** 快档重试间隔（微秒）。 */
#define NETLOG_RETRY_FAST_US (3LL * 1000000LL)

/** 慢档重试间隔（微秒）：失败满 NETLOG_RETRY_FAST_MAX 次后启用。 */
#define NETLOG_RETRY_SLOW_US (30LL * 1000000LL)

/** 第 failures 次失败起切换慢档。 */
#define NETLOG_RETRY_FAST_MAX 5

/** 累计失败达到这个数就放弃：不再重连，关闭整个会话。 */
#define NETLOG_FAIL_GIVE_UP 10

/** 放弃后的关闭缓冲（微秒）：不在 WiFi 事件上下文里直接停机，定时器过渡一拍。 */
#define NETLOG_GIVE_UP_DELAY_US (1LL * 1000000LL)

/** 第 failures 次失败后到下一次动作（重试或关闭）的间隔（微秒）。 */
int64_t netlog_retry_delay_us(int failures);

/** 累计失败是否已到放弃线：到线的下一次动作是关闭会话而不是重连。 */
bool netlog_retry_give_up(int failures);

#ifdef __cplusplus
}
#endif

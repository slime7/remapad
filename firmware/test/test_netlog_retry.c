/**
 * 无线调试会话断线重连节奏（netlog_retry.c）主机端用例：
 * 失败初期 3 秒快档重试、第 5 次失败起退到 30 秒慢档、累计 10 次失败转关闭。
 */
#include "host_test.h"

#include "netlog_retry.h"

static void early_failures_retry_fast(void)
{
  /* 失败 1-4 次：3 秒一档，AP 重启之类的瞬时故障很快能回来。 */
  CHECK_EQ(netlog_retry_delay_us(1), NETLOG_RETRY_FAST_US);
  CHECK_EQ(netlog_retry_delay_us(4), NETLOG_RETRY_FAST_US);
}

static void later_failures_back_off(void)
{
  /* 第 5 次失败起拉长到 30 秒：连不上的持续扫射只剩发热。 */
  CHECK_EQ(netlog_retry_delay_us(5), NETLOG_RETRY_SLOW_US);
  CHECK_EQ(netlog_retry_delay_us(9), NETLOG_RETRY_SLOW_US);
}

static void give_up_closes_session(void)
{
  /* 累计 10 次失败不再重连：隔一小拍整段关闭（缓冲只为了离开 WiFi 事件上下文）。 */
  CHECK(!netlog_retry_give_up(9));
  CHECK(netlog_retry_give_up(10));
  CHECK_EQ(netlog_retry_delay_us(10), NETLOG_GIVE_UP_DELAY_US);
}

HOST_TEST_SUITE(suite_netlog_retry, "netlog_retry", { "失败 1-4 次按 3 秒快档重试", early_failures_retry_fast },
                { "第 5 次失败起退到 30 秒慢档", later_failures_back_off },
                { "累计 10 次失败转关闭不再重连", give_up_closes_session });

/** 无线调试会话断线重连的节奏与放弃线：纯逻辑，接线在 netlog.c，
 *  主机端用例见 firmware/test/test_netlog_retry.c。 */
#include "netlog_retry.h"

int64_t netlog_retry_delay_us(int failures)
{
  if (netlog_retry_give_up(failures)) {
    return NETLOG_GIVE_UP_DELAY_US;
  }
  return failures >= NETLOG_RETRY_FAST_MAX ? NETLOG_RETRY_SLOW_US : NETLOG_RETRY_FAST_US;
}

bool netlog_retry_give_up(int failures)
{
  return failures >= NETLOG_FAIL_GIVE_UP;
}

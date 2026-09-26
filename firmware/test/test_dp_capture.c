/**
 * 主机输出原始采集（dp_capture.c）主机端用例：排队次序、容量上限、截断标志、丢包计数与开关清理。
 */
#include "host_test.h"

#include <string.h>

#include "dp_capture.h"

/** Output Report 0x02 形态的黄金样本：左包状态字 0x7C + 高频纹理。 */
static const uint8_t k_rumble[32] = {
  0x7C, 0x04, 0x80, 0x01, 0x97, 0x63, 0x20, 0x2C, 0x3B, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7C, 0x04, 0x00, 0x01, 0x00, 0x99, 0x20, 0x2C, 0x3B, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** 取一条记录（无记录返回 false），out/slot 可空时不取。 */
static bool pop(uint8_t *payload, size_t cap, size_t *len_out, uint8_t *slot_out)
{
  uint8_t slot = 0;
  const size_t len = dp_capture_pop_payload(payload, cap, &slot);
  if (len_out != NULL) {
    *len_out = len;
  }
  if (slot_out != NULL) {
    *slot_out = slot;
  }
  return len > 0;
}

static void disabled_writes_leave_no_trace(void)
{
  dp_capture_set_enabled(false);
  dp_capture_host_write(DP_CAPTURE_CH_RUMBLE, k_rumble, sizeof(k_rumble));
  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  size_t len = 0;
  CHECK(!pop(payload, sizeof(payload), &len, NULL));

  /* 关闭期间主机照常刷震动（几十 Hz）：这些写入之后也不能突然冒出来。 */
  dp_capture_set_enabled(true);
  CHECK(!pop(payload, sizeof(payload), &len, NULL));
  uint32_t pushed = 0;
  uint32_t dropped = 0;
  dp_capture_counts(&pushed, &dropped);
  CHECK_EQ(pushed, 0U);
  CHECK_EQ(dropped, 0U);
}

static void writes_queue_in_order_with_raw_bytes(void)
{
  dp_capture_set_enabled(true);
  /* 指令通道：0x09/0x07 玩家灯掩码帧（帧头 8 字节 + 1 字节掩码）。 */
  const uint8_t led_cmd[9] = { 0x09, 0x91, 0x01, 0x07, 0x00, 0x01, 0x00, 0x00, 0x01 };
  dp_capture_host_write(DP_CAPTURE_CH_RUMBLE, k_rumble, sizeof(k_rumble));
  dp_capture_host_write(DP_CAPTURE_CH_CMD, led_cmd, sizeof(led_cmd));

  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  size_t len = 0;
  uint8_t slot = 0;
  REQUIRE(pop(payload, sizeof(payload), &len, &slot));
  CHECK_EQ(len, 2u + sizeof(k_rumble));
  CHECK_EQ(slot, 0U);
  const uint8_t want_rumble[2 + sizeof(k_rumble)] = {
    0x12, (uint8_t)sizeof(k_rumble),
    0x7C, 0x04,
    0x80, 0x01,
    0x97, 0x63,
    0x20, 0x2C,
    0x3B, 0x98,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x7C, 0x04,
    0x00, 0x01,
    0x00, 0x99,
    0x20, 0x2C,
    0x3B, 0x98,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
  };
  CHECK_BYTES(payload, want_rumble, sizeof(want_rumble));

  REQUIRE(pop(payload, sizeof(payload), &len, &slot));
  CHECK_EQ(len, 2u + sizeof(led_cmd));
  CHECK_EQ(slot, 1U);
  CHECK_EQ(payload[0], DP_CAPTURE_CH_CMD);
  CHECK_BYTES(&payload[2], led_cmd, sizeof(led_cmd));

  /* 取完即空。 */
  CHECK(!pop(payload, sizeof(payload), &len, NULL));
  dp_capture_set_enabled(false);
}

static void oversized_write_is_truncated_with_flag(void)
{
  dp_capture_set_enabled(true);
  /* 升级数据块的写入可到 MTU 上限（几百字节）：留头去尾并带截断标记。 */
  uint8_t big[300];
  for (size_t i = 0; i < sizeof(big); i++) {
    big[i] = (uint8_t)(i * 7);
  }
  dp_capture_host_write(DP_CAPTURE_CH_FWUPG, big, sizeof(big));

  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  size_t len = 0;
  REQUIRE(pop(payload, sizeof(payload), &len, NULL));
  CHECK_EQ(len, 2u + DP_CAPTURE_MAX_DATA);
  CHECK_EQ(payload[0], DP_CAPTURE_CH_FWUPG);
  CHECK_EQ(payload[1], (unsigned)(DP_CAPTURE_MAX_DATA | 0x80u));
  CHECK_BYTES(&payload[2], big, DP_CAPTURE_MAX_DATA);
  dp_capture_set_enabled(false);
}

static void full_queue_drops_newest_and_keeps_queued(void)
{
  dp_capture_set_enabled(true);
  /* 写满整条队列再补一条：后来的丢弃，已排队的原样按序保留。 */
  const uint8_t sample[4] = { 0x31, 0x02, 0x00, 0x00 };
  for (size_t i = 0; i < DP_CAPTURE_QUEUE_LEN; i++) {
    dp_capture_host_write(DP_CAPTURE_CH_CMD, sample, sizeof(sample));
  }
  dp_capture_host_write(DP_CAPTURE_CH_RUMBLE, k_rumble, sizeof(k_rumble));

  uint32_t pushed = 0;
  uint32_t dropped = 0;
  dp_capture_counts(&pushed, &dropped);
  CHECK_EQ(pushed, (uint32_t)DP_CAPTURE_QUEUE_LEN);
  CHECK_EQ(dropped, 1U);

  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  size_t len = 0;
  uint8_t slot = 0;
  for (size_t i = 0; i < DP_CAPTURE_QUEUE_LEN; i++) {
    REQUIRE(pop(payload, sizeof(payload), &len, &slot));
    CHECK_EQ(payload[0], DP_CAPTURE_CH_CMD);
    CHECK_EQ(slot, i);
  }
  CHECK(!pop(payload, sizeof(payload), &len, NULL));
  dp_capture_set_enabled(false);
}

static void re_enabling_starts_from_a_clean_state(void)
{
  dp_capture_set_enabled(true);
  dp_capture_host_write(DP_CAPTURE_CH_CMD, (const uint8_t *)"\x07", 1);
  dp_capture_set_enabled(false);
  dp_capture_set_enabled(true);

  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  size_t len = 0;
  uint8_t slot = 0;
  CHECK(!pop(payload, sizeof(payload), &len, &slot));
  uint32_t pushed = 0;
  uint32_t dropped = 0;
  dp_capture_counts(&pushed, &dropped);
  CHECK_EQ(pushed, 0U);
  CHECK_EQ(dropped, 0U);
  /* 记录号也从 0 重新起算：每次采集的 seq 各自独立。 */
  dp_capture_host_write(DP_CAPTURE_CH_CMD, (const uint8_t *)"\x07", 1);
  REQUIRE(pop(payload, sizeof(payload), &len, &slot));
  CHECK_EQ(slot, 0U);
  dp_capture_set_enabled(false);
}

static void undersized_buffer_keeps_record_queued(void)
{
  dp_capture_set_enabled(true);
  dp_capture_host_write(DP_CAPTURE_CH_CMD, (const uint8_t *)"\x09\x91", 2);
  /* 缓冲连载荷头都装不下：记录必须留在队首，不能被悄悄丢掉。 */
  uint8_t tiny[1] = { 0 };
  size_t len = dp_capture_pop_payload(tiny, sizeof(tiny), NULL);
  CHECK_EQ(len, 0U);
  uint8_t payload[2 + DP_CAPTURE_MAX_DATA];
  REQUIRE(pop(payload, sizeof(payload), &len, NULL));
  CHECK_EQ(len, 4U);
  dp_capture_set_enabled(false);
}

HOST_TEST_SUITE(suite_dp_capture, "dp_capture 主机输出原始采集",
                { "采集关闭时主机写入不留痕", disabled_writes_leave_no_trace },
                { "开启后主机写入按原样按序排队", writes_queue_in_order_with_raw_bytes },
                { "超过单帧上限的写入截断并带标记", oversized_write_is_truncated_with_flag },
                { "队列写满丢弃后来的写入，已排队的原样保留", full_queue_drops_newest_and_keeps_queued },
                { "重新开启采集从干净现场开始", re_enabling_starts_from_a_clean_state },
                { "缓冲不足时记录留在队列里", undersized_buffer_keeps_record_queued });

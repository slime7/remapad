/**
 * 手柄固件更新的记录流装配（ns2_upgrade.c）主机端用例：期望值与字节取自设备采集的更新流样本，
 * 装配错位会让伪装应答答错帧、帧长判断错会让整包推不完。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_upgrade.h"

/** 样本：首帧装配流的前 320 字节（8 字节帧头 + 帧体开头）。 */
static const uint8_t k_frame_head[320] = {
  0x0d, 0x91, 0x01, 0x04, 0x10, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x64, 0xaa, 0x20, 0x53, 0x59,
  0x53, 0xd5, 0x22, 0xbd, 0x15, 0x00, 0x03, 0xe7, 0x10, 0x83, 0xff, 0xec, 0x07, 0x46, 0xec, 0xf4, 0xd9, 0x17, 0x2a,
  0xe5, 0xae, 0x0b, 0x5d, 0x6b, 0xcf, 0x12, 0x4a, 0xd5, 0x4c, 0x4d, 0x83, 0xb0, 0x71, 0xbd, 0xe7, 0x01, 0xf2, 0xcc,
  0xb5, 0x8b, 0x45, 0x0c, 0x76, 0x7e, 0x42, 0x81, 0x4b, 0xcb, 0x0d, 0x28, 0x43, 0xcc, 0x6d, 0xca, 0xe7, 0xa1, 0xab,
  0xda, 0xc4, 0x80, 0x37, 0x4b, 0xf7, 0xe9, 0x8d, 0xa6, 0x5d, 0x1e, 0x7c, 0xe3, 0x67, 0xe1, 0x71, 0x8f, 0xe1, 0x98,
  0xd3, 0xb2, 0x1f, 0x35, 0x6b, 0xf1, 0x87, 0x9b, 0xd2, 0x73, 0x17, 0x77, 0xcc, 0xa9, 0x9a, 0xc2, 0xb8, 0x30, 0x6d,
  0x19, 0xe8, 0xf9, 0xba, 0x6c, 0xee, 0xae, 0x54, 0x5d, 0x65, 0xc6, 0x20, 0x8f, 0xd3, 0xd0, 0xba, 0x73, 0xf3, 0x4b,
  0x77, 0x1e, 0x10, 0x31, 0x8c, 0xe3, 0x0c, 0x83, 0x95, 0xa6, 0x9e, 0x09, 0xa8, 0x6d, 0x8b, 0x12, 0x1e, 0xd9, 0xdf,
  0xa6, 0x81, 0xe6, 0x1e, 0xa2, 0xd2, 0xe0, 0x6f, 0x8d, 0xfc, 0x65, 0xf4, 0x08, 0x67, 0x62, 0x03, 0xa0, 0x0d, 0x1d,
  0xc3, 0xb1, 0xd6, 0xa0, 0x07, 0xb4, 0x5f, 0x40, 0xf3, 0x47, 0x8f, 0x8e, 0x67, 0x92, 0x22, 0xb1, 0xd3, 0x50, 0x20,
  0x35, 0x87, 0x68, 0xb7, 0x73, 0x06, 0x1c, 0xb1, 0xbc, 0x7d, 0x7e, 0x31, 0xe9, 0x48, 0x3d, 0x49, 0x51, 0x27, 0x76,
  0xe4, 0x11, 0x22, 0x74, 0x92, 0x80, 0x1f, 0x91, 0xa3, 0x29, 0x74, 0xf8, 0x97, 0x48, 0x01, 0xd8, 0xb3, 0xbc, 0xf7,
  0xee, 0x79, 0x97, 0x3b, 0x35, 0x06, 0x41, 0x45, 0xde, 0xdc, 0x3a, 0x1e, 0xef, 0x16, 0xa7, 0xd6, 0xfb, 0xdd, 0xdd,
  0xe6, 0xf0, 0x07, 0x1b, 0x46, 0x56, 0xeb, 0x37, 0xae, 0x5b, 0x8e, 0xc4, 0xd3, 0x6a, 0xce, 0xd7, 0x1d, 0x9c, 0x38,
  0x11, 0xa1, 0x3e, 0xb9, 0x9c, 0x3f, 0xe6, 0x45, 0x17, 0x9b, 0xa7, 0x52, 0x7b, 0x0b, 0x6c, 0x86, 0x68, 0x0b, 0xc1,
  0x79, 0xae, 0x54, 0xa7, 0x74, 0xe4, 0x95, 0x19, 0x68, 0x48, 0xa5, 0x2b, 0x9a, 0x23, 0x7a, 0xfe, 0x85, 0xda, 0x22,
  0x48, 0xe1, 0x42, 0x16, 0x21, 0x5b, 0xba, 0x5f, 0xe4, 0xd3, 0x03, 0xd5, 0xd8, 0x78, 0xb7, 0x13,
};

/** 样本事实：首帧 4108 字节，帧头声明体长 0x1004，体首 4 字节是块大小 0x1000。 */
#define CAPTURE_FRAME_LEN 4108u
#define CAPTURE_FRAME_BODY 0x1004u
#define CAPTURE_BLOCK_SIZE 0x1000u
/** 主机每条记录携带的载荷字节数（首帧 41 条满记录 + 1 条 8 字节收尾）。 */
#define RECORD_PAYLOAD 100u

/** 按主机的切法把一帧拆成记录喂进去（首条类型 0x01，其后 0x02），返回报完成次数。 */
static unsigned feed_frame(ns2_upgrade_t *up, const uint8_t *data, size_t len)
{
  unsigned frames = 0;
  uint8_t record[NS2_UPGRADE_RECORD_HEADER_LEN + RECORD_PAYLOAD];
  size_t off = 0;
  unsigned index = 0;
  while (off < len) {
    size_t chunk = len - off;
    if (chunk > RECORD_PAYLOAD) {
      chunk = RECORD_PAYLOAD;
    }
    record[0] = off == 0 ? NS2_UPGRADE_RECORD_FIRST : NS2_UPGRADE_RECORD_NEXT;
    record[1] = (uint8_t)index;
    record[2] = (uint8_t)(chunk & 0xFFu);
    record[3] = (uint8_t)(chunk >> 8);
    memcpy(&record[NS2_UPGRADE_RECORD_HEADER_LEN], &data[off], chunk);
    if (ns2_upgrade_feed(up, record, chunk + NS2_UPGRADE_RECORD_HEADER_LEN) == NS2_UPGRADE_FRAME) {
      frames++;
    }
    index++;
    off += chunk;
  }
  return frames;
}

static size_t le32(const uint8_t *data)
{
  return (size_t)data[0] | ((size_t)data[1] << 8) | ((size_t)data[2] << 16) | ((size_t)data[3] << 24);
}

static void first_frame_assembles_into_one_command_frame(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  uint8_t frame[CAPTURE_FRAME_LEN];
  memcpy(frame, k_frame_head, sizeof(k_frame_head));
  for (size_t i = sizeof(k_frame_head); i < sizeof(frame); i++) {
    frame[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
  }

  CHECK_EQ(feed_frame(&up, frame, sizeof(frame)), 1u);
  CHECK_EQ(up.frames, 1u);
  CHECK_EQ(up.records, 42u);
  CHECK_EQ(up.bytes, 42u * (RECORD_PAYLOAD + NS2_UPGRADE_RECORD_HEADER_LEN) - (RECORD_PAYLOAD - 8u));
  CHECK_EQ(up.min_record, 12u);
  CHECK_EQ(up.max_record, 104u);
  CHECK_EQ(up.frame_len, CAPTURE_FRAME_LEN);
  CHECK_EQ(ns2_upgrade_frame_body(&up), CAPTURE_FRAME_BODY);
  CHECK_EQ(le32(&up.frame[NS2_FRAME_HEADER_LEN]), CAPTURE_BLOCK_SIZE);
  CHECK_EQ(up.frame[0], 0x0D);
  CHECK_EQ(up.frame[3], 0x04);
  CHECK_EQ(up.sample_len, CAPTURE_FRAME_LEN);
  CHECK_BYTES(up.sample, frame, sizeof(frame));
}

static void frame_stays_open_until_the_declared_body_arrives(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  uint8_t frame[CAPTURE_FRAME_LEN];
  memcpy(frame, k_frame_head, sizeof(k_frame_head));
  memset(&frame[sizeof(k_frame_head)], 0, sizeof(frame) - sizeof(k_frame_head));

  /* 少喂最后一条 8 字节记录：帧头说体长 4100，此时不该报完成。 */
  CHECK_EQ(feed_frame(&up, frame, sizeof(frame) - 8u), 0u);
  CHECK_EQ(up.frames, 0u);
  CHECK_EQ(up.frame_len, CAPTURE_FRAME_LEN - 8u);
  CHECK_EQ(ns2_upgrade_frame_body(&up), CAPTURE_FRAME_BODY);
  CHECK_EQ(up.sample_len, 0u);
}

static void short_record_is_counted_but_not_assembled(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  const uint8_t short_record[3] = { 0x01, 0x00, 0x64 };
  CHECK_EQ(ns2_upgrade_feed(&up, short_record, sizeof(short_record)), NS2_UPGRADE_MALFORMED);
  CHECK_EQ(up.records, 1u);
  CHECK_EQ(up.bytes, 3u);
  CHECK_EQ(up.frame_len, 0u);
  CHECK_EQ(up.frames, 0u);
}

static void frame_start_record_restarts_assembly(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  /* 先来一条续帧记录（上一帧的收尾没收到）：按续帧累加，不报完成。 */
  const uint8_t next[6] = { 0x02, 0x01, 0x02, 0x00, 0xAA, 0x55 };
  CHECK_EQ(ns2_upgrade_feed(&up, next, sizeof(next)), NS2_UPGRADE_NONE);
  CHECK_EQ(up.frame_len, 2u);
  CHECK_EQ(up.frame[0], 0xAA);

  /* 帧首记录一到，装配从这一条重新开始，前面的残片不会被拼进来。 */
  const uint8_t first[12] = { 0x01, 0x00, 0x08, 0x00, 0x0D, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00 };
  CHECK_EQ(ns2_upgrade_feed(&up, first, sizeof(first)), NS2_UPGRADE_NONE);
  CHECK_EQ(up.frame_len, 8u);
  CHECK_EQ(up.frame[0], 0x0D);
  CHECK_EQ(up.frames, 0u);
}

static void completed_frame_gives_way_to_the_next(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  uint8_t record[NS2_UPGRADE_RECORD_HEADER_LEN + 16];
  record[0] = 0x01;
  record[1] = 0x00;
  record[2] = 0x10;
  record[3] = 0x00;
  const uint8_t body[16] = { 0x0D, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00,
                             0x00, 0x10, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44 };
  memcpy(&record[NS2_UPGRADE_RECORD_HEADER_LEN], body, sizeof(body));
  CHECK_EQ(ns2_upgrade_feed(&up, record, sizeof(record)), NS2_UPGRADE_FRAME);
  CHECK_EQ(up.frames, 1u);

  const uint8_t next[12] = { 0x02, 0x01, 0x08, 0x00, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC };
  CHECK_EQ(ns2_upgrade_feed(&up, next, sizeof(next)), NS2_UPGRADE_NONE);
  CHECK_EQ(up.frames, 1u);
  CHECK_EQ(up.frame_len, 8u);
  CHECK_EQ(up.frame[0], 0x55);
}

static void frame_over_the_buffer_never_completes(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  const uint8_t oversized[12] = { 0x01, 0x00, 0x08, 0x00, 0x0D, 0x91, 0x01, 0x04, 0xFF, 0xFF, 0x00, 0x00 };
  CHECK_EQ(ns2_upgrade_feed(&up, oversized, sizeof(oversized)), NS2_UPGRADE_NONE);
  CHECK(up.truncated);

  uint8_t filler[RECORD_PAYLOAD + NS2_UPGRADE_RECORD_HEADER_LEN];
  memset(filler, 0x5A, sizeof(filler));
  filler[0] = 0x02;
  filler[1] = 0x01;
  filler[2] = (uint8_t)RECORD_PAYLOAD;
  filler[3] = 0x00;
  CHECK_EQ(ns2_upgrade_feed(&up, filler, sizeof(filler)), NS2_UPGRADE_NONE);
  CHECK_EQ(up.frames, 0u);
}

static void tail_sample_keeps_the_most_recent_bytes(void)
{
  ns2_upgrade_t up;
  ns2_upgrade_reset(&up);

  uint8_t record[NS2_UPGRADE_RECORD_HEADER_LEN + RECORD_PAYLOAD];
  for (unsigned i = 0; i < 5; i++) {
    record[0] = i == 0 ? NS2_UPGRADE_RECORD_FIRST : NS2_UPGRADE_RECORD_NEXT;
    record[1] = (uint8_t)i;
    record[2] = (uint8_t)RECORD_PAYLOAD;
    record[3] = 0x00;
    memset(&record[NS2_UPGRADE_RECORD_HEADER_LEN], (int)(0x10u + i), RECORD_PAYLOAD);
    ns2_upgrade_feed(&up, record, sizeof(record));
  }
  CHECK_EQ(up.tail_len, NS2_UPGRADE_TAIL_CAP);
  CHECK_EQ(up.tail[NS2_UPGRADE_TAIL_CAP - 1u], 0x14);
  CHECK_EQ(up.tail[NS2_UPGRADE_TAIL_CAP - RECORD_PAYLOAD], 0x14);
  /* 留样只保留最近一窗：窗口起点落在第 3 条记录的载荷中部（第 1、2 条
     * 记录与第 3 条的前半已被挤出去）。 */
  CHECK_EQ(up.tail[0], 0x12);
}

HOST_TEST_SUITE(suite_ns2_upgrade, "ns2_upgrade",
                { "主机推送的首帧按记录装配成一条 0x0d/0x04 命令帧", first_frame_assembles_into_one_command_frame },
                { "帧体没到齐之前不报完成", frame_stays_open_until_the_declared_body_arrives },
                { "记录头不完整的写入只计数不装配", short_record_is_counted_but_not_assembled },
                { "帧首记录一到装配就从头开始", frame_start_record_restarts_assembly },
                { "一帧凑齐后下一条记录起算新的一帧", completed_frame_gives_way_to_the_next },
                { "超过装配缓冲的帧永不报完成", frame_over_the_buffer_never_completes },
                { "尾部留样只保留最近一窗记录", tail_sample_keeps_the_most_recent_bytes });

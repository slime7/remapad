/**
 * amiibo 镜像上传会话（amiibo_proto.c）的主机端用例：PC 经桥接帧把一份
 * NTAG215 镜像传进设备，这些用例钉住「三帧数据拼出完整镜像落库、缺帧不落
 * 库可续传、坏尺寸与空槽位当场报错」的会话行为。
 */
#include "host_test.h"

#include <stdio.h>
#include <string.h>

#include "amiibo_proto.h"

/** 540 字节的花样镜像（每字节 = 页内偏移取反），便于逐字节比对。 */
static void fill_pattern(uint8_t *out, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    out[i] = (uint8_t)(i ^ 0xA5u);
  }
}

/** 构造 BEGIN 载荷：name_len + name + size(u32 LE)。 */
static size_t make_begin(uint8_t *out, const char *name, uint32_t size)
{
  const size_t name_len = strlen(name);
  out[0] = (uint8_t)name_len;
  memcpy(&out[1], name, name_len);
  out[1 + name_len] = (uint8_t)(size & 0xFFu);
  out[2 + name_len] = (uint8_t)((size >> 8) & 0xFFu);
  out[3 + name_len] = (uint8_t)((size >> 16) & 0xFFu);
  out[4 + name_len] = (uint8_t)((size >> 24) & 0xFFu);
  return 1 + name_len + 4;
}

/** 构造 DATA 载荷：offset(u16 LE) + 数据。 */
static size_t make_data(uint8_t *out, uint16_t offset, const uint8_t *chunk, size_t len)
{
  out[0] = (uint8_t)(offset & 0xFFu);
  out[1] = (uint8_t)(offset >> 8);
  memcpy(&out[2], chunk, len);
  return 2 + len;
}

/** 落库回调的捕获记录。 */
static char s_stored_name[AMIIBO_NAME_MAX + 1];
static uint8_t s_stored_data[NS2_NFC_IMAGE_MAX];
static size_t s_stored_len;
static int s_stored_slot;
static int s_store_calls;
static int s_store_next_slot;

static void reset_capture(void)
{
  s_stored_name[0] = 0;
  memset(s_stored_data, 0, sizeof(s_stored_data));
  s_stored_len = 0;
  s_stored_slot = -1;
  s_store_calls = 0;
  s_store_next_slot = 2;
}

static int store_capture(const char *name, const uint8_t *data, size_t len, void *user)
{
  (void)user;
  s_store_calls++;
  snprintf(s_stored_name, sizeof(s_stored_name), "%s", name);
  if (len > sizeof(s_stored_data)) {
    return -1;
  }
  memcpy(s_stored_data, data, len);
  s_stored_len = len;
  s_stored_slot = s_store_next_slot;
  return s_store_next_slot;
}

/** 三帧数据按偏移拼出完整 540 字节镜像，END 落库并回 DONE。 */
static void test_upload_completes_and_stores(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();

  uint8_t payload[2 + AMIIBO_DATA_MAX_LEN];
  uint8_t image[NS2_NFC_TAG_SIZE];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Alm", sizeof(image));
  const amiibo_upload_result_t b = amiibo_upload_begin(&up, begin, begin_len, 1000);
  CHECK_EQ(b.state, AMIIBO_STATE_RECEIVING);
  CHECK_EQ(b.code, AMIIBO_CODE_OK);
  CHECK(b.reply);

  size_t off = 0;
  for (size_t i = 0; i < 3; i++) {
    const size_t chunk = sizeof(image) - off > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : sizeof(image) - off;
    const size_t len = make_data(payload, (uint16_t)off, &image[off], chunk);
    const amiibo_upload_result_t d = amiibo_upload_data(&up, payload, len, 2000 + (int64_t)i);
    CHECK_EQ(d.state, AMIIBO_STATE_RECEIVING);
    CHECK_EQ(d.code, AMIIBO_CODE_OK);
    off += chunk;
    CHECK_EQ(d.received, off);
  }

  const amiibo_upload_result_t e = amiibo_upload_end(&up, 3000, store_capture, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_DONE);
  CHECK_EQ(e.code, AMIIBO_CODE_OK);
  CHECK(e.finished);
  CHECK_EQ(e.slot, 2);
  CHECK_EQ(s_store_calls, 1);
  CHECK(strcmp(s_stored_name, "Alm") == 0);
  CHECK_EQ(s_stored_len, NS2_NFC_TAG_SIZE);
  CHECK_BYTES(s_stored_data, image, NS2_NFC_TAG_SIZE);
  CHECK(!amiibo_upload_busy(&up));
}

/** 尺寸不是一整份 NTAG215 镜像的上传在 BEGIN 就被拒绝，会话保持空闲。 */
static void test_begin_rejects_wrong_size(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Alm", 512);
  const amiibo_upload_result_t b = amiibo_upload_begin(&up, begin, begin_len, 1000);
  CHECK_EQ(b.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(b.code, AMIIBO_CODE_BAD_HEADER);
  CHECK_EQ(b.slot, -1);
  CHECK(!amiibo_upload_busy(&up));

  /* 拒绝之后同一个会话还能正常开始。 */
  const size_t ok_len = make_begin(begin, "Alm", NS2_NFC_TAG_SIZE);
  const amiibo_upload_result_t ok = amiibo_upload_begin(&up, begin, ok_len, 2000);
  CHECK_EQ(ok.state, AMIIBO_STATE_RECEIVING);
  CHECK(amiibo_upload_busy(&up));
}

/** 572 字节（镜像 + 尾部 32 字节厂商签名）同样收满落库：签名段原样交给
 *  存储回调，进设备读缓冲头区。 */
static void test_upload_572_with_signature(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();

  uint8_t image[NS2_NFC_IMAGE_MAX];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Bokoblin", sizeof(image));
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 1000).state == AMIIBO_STATE_RECEIVING);

  uint8_t payload[2 + AMIIBO_DATA_MAX_LEN];
  size_t off = 0;
  while (off < sizeof(image)) {
    const size_t chunk = sizeof(image) - off > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : sizeof(image) - off;
    const size_t len = make_data(payload, (uint16_t)off, &image[off], chunk);
    REQUIRE(amiibo_upload_data(&up, payload, len, 1001 + (int64_t)off).code == AMIIBO_CODE_OK);
    off += chunk;
  }
  const amiibo_upload_result_t e = amiibo_upload_end(&up, 2000, store_capture, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_DONE);
  CHECK_EQ(s_stored_len, NS2_NFC_IMAGE_MAX);
  CHECK_BYTES(s_stored_data, image, NS2_NFC_IMAGE_MAX);

  /* 572 的会话里发到 540 就 END 是尺寸不符（签名段缺失）。 */
  amiibo_upload_init(&up);
  reset_capture();
  const size_t short_len = make_begin(begin, "Bokoblin", NS2_NFC_IMAGE_MAX);
  REQUIRE(amiibo_upload_begin(&up, begin, short_len, 3000).state == AMIIBO_STATE_RECEIVING);
  size_t sent = 0;
  while (sent < NS2_NFC_TAG_SIZE) {
    const size_t chunk = NS2_NFC_TAG_SIZE - sent > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : NS2_NFC_TAG_SIZE - sent;
    const size_t len = make_data(payload, (uint16_t)sent, &image[sent], chunk);
    REQUIRE(amiibo_upload_data(&up, payload, len, 3001 + (int64_t)sent).code == AMIIBO_CODE_OK);
    sent += chunk;
  }
  const amiibo_upload_result_t short_end = amiibo_upload_end(&up, 4000, store_capture, NULL);
  CHECK_EQ(short_end.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(short_end.code, AMIIBO_CODE_SIZE_MISMATCH);
  CHECK_EQ(s_store_calls, 0);
}

/** 空名字与超长名字的 BEGIN 都是坏头。 */
static void test_begin_rejects_bad_name(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);

  uint8_t begin[1 + AMIIBO_NAME_MAX + 2 + 4];
  const size_t empty_len = make_begin(begin, "", NS2_NFC_TAG_SIZE);
  const amiibo_upload_result_t empty = amiibo_upload_begin(&up, begin, empty_len, 1000);
  CHECK_EQ(empty.code, AMIIBO_CODE_BAD_HEADER);

  /* 32 字节名字比上限多 1。 */
  const size_t long_len = make_begin(begin, "0123456789abcdefghij0123456789ab", NS2_NFC_TAG_SIZE);
  const amiibo_upload_result_t long_name = amiibo_upload_begin(&up, begin, long_len, 1000);
  CHECK_EQ(long_name.code, AMIIBO_CODE_BAD_HEADER);
}

/** 数据缺一块时 END 不落库；补上缺口后续传到 END 才算完成。 */
static void test_missing_chunk_stores_nothing_and_resumes(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();
  uint8_t image[NS2_NFC_TAG_SIZE];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Metroid", sizeof(image));
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 1000).state == AMIIBO_STATE_RECEIVING);

  uint8_t payload[2 + AMIIBO_DATA_MAX_LEN];
  /* 只发第一块，跳过第二块，直接发第三块：偏移对不上要报 OFFSET_ERROR。 */
  const size_t first_len = make_data(payload, 0, image, AMIIBO_DATA_MAX_LEN);
  REQUIRE(amiibo_upload_data(&up, payload, first_len, 1001).code == AMIIBO_CODE_OK);
  const size_t third_len = make_data(payload, 2 * AMIIBO_DATA_MAX_LEN, &image[2 * AMIIBO_DATA_MAX_LEN],
                                     sizeof(image) - 2 * AMIIBO_DATA_MAX_LEN);
  const amiibo_upload_result_t gap = amiibo_upload_data(&up, payload, third_len, 1002);
  CHECK_EQ(gap.code, AMIIBO_CODE_OFFSET_ERROR);
  CHECK_EQ(gap.received, AMIIBO_DATA_MAX_LEN);

  const amiibo_upload_result_t early_end = amiibo_upload_end(&up, 1003, store_capture, NULL);
  CHECK_EQ(early_end.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(early_end.code, AMIIBO_CODE_SIZE_MISMATCH);
  CHECK_EQ(s_store_calls, 0);
  CHECK(!amiibo_upload_busy(&up));

  /* 重新开始并按序传完：ACK 的 received 就是续传起点。 */
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 2000).state == AMIIBO_STATE_RECEIVING);
  size_t off = 0;
  while (off < sizeof(image)) {
    const size_t chunk = sizeof(image) - off > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : sizeof(image) - off;
    const size_t len = make_data(payload, (uint16_t)off, &image[off], chunk);
    const amiibo_upload_result_t d = amiibo_upload_data(&up, payload, len, 2000 + (int64_t)off);
    CHECK_EQ(d.code, AMIIBO_CODE_OK);
    off += chunk;
  }
  const amiibo_upload_result_t e = amiibo_upload_end(&up, 5000, store_capture, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_DONE);
  CHECK_EQ(s_store_calls, 1);
}

/** 槽位写满（存储回调失败）时 END 报存储错误，会话退回空闲。 */
static void test_store_failure_reports_store_error(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  uint8_t image[NS2_NFC_TAG_SIZE];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Samus", sizeof(image));
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 1000).state == AMIIBO_STATE_RECEIVING);

  uint8_t payload[2 + AMIIBO_DATA_MAX_LEN];
  size_t off = 0;
  while (off < sizeof(image)) {
    const size_t chunk = sizeof(image) - off > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : sizeof(image) - off;
    const size_t len = make_data(payload, (uint16_t)off, &image[off], chunk);
    REQUIRE(amiibo_upload_data(&up, payload, len, 1001 + (int64_t)off).code == AMIIBO_CODE_OK);
    off += chunk;
  }
  const amiibo_upload_result_t e = amiibo_upload_end(&up, 2000, NULL, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(e.code, AMIIBO_CODE_STORE_ERROR);
  CHECK_EQ(e.slot, -1);
  CHECK(!amiibo_upload_busy(&up));
}

/** 接收中空闲超时作废会话；之后到的数据帧不再被接收。 */
static void test_idle_session_times_out(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();
  uint8_t image[NS2_NFC_TAG_SIZE];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Tiki", sizeof(image));
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 1000).state == AMIIBO_STATE_RECEIVING);

  const amiibo_upload_result_t t = amiibo_upload_tick(&up, 1000 + AMIIBO_SESSION_TIMEOUT_US + 1);
  CHECK_EQ(t.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(t.code, AMIIBO_CODE_TIMEOUT);
  CHECK(!amiibo_upload_busy(&up));

  uint8_t payload[2 + 4];
  const size_t len = make_data(payload, 0, image, 4);
  const amiibo_upload_result_t late = amiibo_upload_data(&up, payload, len, 1000 + 2 * AMIIBO_SESSION_TIMEOUT_US);
  CHECK_EQ(late.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(late.code, AMIIBO_CODE_OFFSET_ERROR);
}

/** 没有 BEGIN 的数据帧与结束帧都被拒绝，不打乱后续会话。 */
static void test_data_needs_an_open_session(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();

  uint8_t payload[2 + 4] = { 0 };
  const amiibo_upload_result_t d = amiibo_upload_data(&up, payload, sizeof(payload), 1000);
  CHECK_EQ(d.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(d.code, AMIIBO_CODE_OFFSET_ERROR);

  const amiibo_upload_result_t e = amiibo_upload_end(&up, 1001, store_capture, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_FAILED);
  CHECK_EQ(s_store_calls, 0);
}

/** ACK 载荷是定长黄金字节：state + code + received(u32 LE) + slot。 */
static void test_ack_payload_is_golden(void)
{
  const amiibo_upload_result_t r = {
    .state = AMIIBO_STATE_DONE,
    .code = AMIIBO_CODE_OK,
    .received = 540,
    .slot = 3,
    .reply = true,
    .finished = true,
  };
  uint8_t ack[AMIIBO_ACK_PAYLOAD_LEN];
  CHECK_EQ(amiibo_upload_encode_ack(&r, ack, sizeof(ack)), AMIIBO_ACK_PAYLOAD_LEN);
  const uint8_t golden[AMIIBO_ACK_PAYLOAD_LEN] = { 0x02, 0x00, 0x1C, 0x02, 0x00, 0x00, 0x03 };
  CHECK_BYTES(ack, golden, AMIIBO_ACK_PAYLOAD_LEN);
}

/** ACK 在共享串口上被挤掉时 PC 整段重发：已收区间的重复帧按幂等处理。 */
static void test_duplicate_data_frames_are_idempotent(void)
{
  amiibo_upload_t up;
  amiibo_upload_init(&up);
  reset_capture();
  uint8_t image[NS2_NFC_TAG_SIZE];
  fill_pattern(image, sizeof(image));

  uint8_t begin[1 + AMIIBO_NAME_MAX + 4];
  const size_t begin_len = make_begin(begin, "Loot", sizeof(image));
  REQUIRE(amiibo_upload_begin(&up, begin, begin_len, 1000).state == AMIIBO_STATE_RECEIVING);

  uint8_t payload[2 + AMIIBO_DATA_MAX_LEN];
  const size_t first_len = make_data(payload, 0, image, AMIIBO_DATA_MAX_LEN);
  REQUIRE(amiibo_upload_data(&up, payload, first_len, 1001).code == AMIIBO_CODE_OK);
  const amiibo_upload_result_t dup = amiibo_upload_data(&up, payload, first_len, 1002);
  CHECK_EQ(dup.code, AMIIBO_CODE_OK);
  CHECK_EQ(dup.received, AMIIBO_DATA_MAX_LEN);

  /* 继续传完，落库内容不受重复帧影响。 */
  size_t off = AMIIBO_DATA_MAX_LEN;
  while (off < sizeof(image)) {
    const size_t chunk = sizeof(image) - off > AMIIBO_DATA_MAX_LEN ? AMIIBO_DATA_MAX_LEN : sizeof(image) - off;
    const size_t len = make_data(payload, (uint16_t)off, &image[off], chunk);
    REQUIRE(amiibo_upload_data(&up, payload, len, 1003 + (int64_t)off).code == AMIIBO_CODE_OK);
    off += chunk;
  }
  const amiibo_upload_result_t e = amiibo_upload_end(&up, 2000, store_capture, NULL);
  CHECK_EQ(e.state, AMIIBO_STATE_DONE);
  CHECK_BYTES(s_stored_data, image, NS2_NFC_TAG_SIZE);
}

HOST_TEST_SUITE(suite_amiibo_proto, "amiibo_proto",
                { "三帧数据按偏移拼出完整镜像后落库并回 DONE", test_upload_completes_and_stores },
                { "尺寸不是 540 字节的上传在 BEGIN 就被拒绝", test_begin_rejects_wrong_size },
                { "572 字节（带厂商签名）同样收满落库，缺签名段算尺寸不符", test_upload_572_with_signature },
                { "空名字与超长名字的 BEGIN 都是坏头", test_begin_rejects_bad_name },
                { "数据缺一块时 END 不落库，补上缺口后续传完成", test_missing_chunk_stores_nothing_and_resumes },
                { "槽位落库失败时报存储错误并退回空闲", test_store_failure_reports_store_error },
                { "接收中空闲超时作废会话", test_idle_session_times_out },
                { "没有 BEGIN 的数据帧与结束帧都被拒绝", test_data_needs_an_open_session },
                { "ACK 丢失后重发的已收帧按幂等处理", test_duplicate_data_frames_are_idempotent },
                { "ACK 载荷是定长黄金字节", test_ack_payload_is_golden });

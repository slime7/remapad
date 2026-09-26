#include "ns2_nfc.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ns2_frames.h"

static const char *TAG = "remapad_ns2nfc";

/** 0x14（读取中）→ 0x15（数据就绪）的模拟耗时：30ms 时主机能一气拉完全部 8 块
 *  （15ms 时反而 3 块就停）；块间的 0x05 轮询第一拍读到「读取中」、第二拍读到「就绪」。 */
#define NS2_NFC_STAGE_READY_DELAY_US (30 * 1000LL)

/** 0x05 卡信息体的前缀：`09 00 00 00`（状态区）+ `01 01 02 00`
 *  （感应标志 / 协议 / 卡类型 02 = Type 2 标签）+ `07`（UID 长度）。 */
static const uint8_t s_tag_info_prefix[9] = { 0x09, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x00, 0x07 };

/** 0x0C 的 NFC 控制器状态体（固定原值）。 */
static const uint8_t s_nfc_status[4] = NS2_NFC_STATUS_BODY;

static struct {
  /** 预置的标签镜像（heap 优先 PSRAM），空指针 = 感应区没有卡。 */
  uint8_t *image;
  size_t image_len;
  /** 厂商签名（READ_SIG 页）：572 字节 dump 尾部；未带签名时全零。 */
  uint8_t sig[NS2_NFC_SIG_SIZE];
  /** 主机已开射频场轮询（0x01/0x03）；串口 CLI 也能手动开关做验证。 */
  bool polling;
  /** 读卡流程状态：0 = 未在读书（卡片在场 0x09）；0x14 = 读取中（0x06 触发后）；
     *  0x15 = 数据就绪（读卡耗时过后）——主机在 0x06 后检查 0x05 体首字节，
     *  并等报告状态字节到 0x15 才来拉数据。 */
  uint8_t read_stage;
  /** 主机抽完块（EOF 探测）后的「读取结束」：状态报 s_drained_state（串口
     *  可调，默认 0x00 Idle），下一次 0x03/0x04/0x06 复位。 */
  bool drained;
  int64_t read_started_us;
  /** 写卡缓冲：主机经 0x14 分块装载，0x01/0x08 一次性提交进镜像。
     *  首块带 `d0 07` 操作描述符时进入流式模式（描述符 17 字节剥掉，标签
     *  数据从页 4 接续写入）；否则按偏移直写（旧假设）。 */
  uint8_t write_buf[NS2_NFC_TAG_SIZE];
  bool write_dirty;
  bool write_stream;
  size_t write_pos;
  ns2_nfc_write_sink_fn sink;
  void *sink_user;
} s_nfc;

/** 0x14 首块的操作描述符长度：`d0 07` + UID(7) + 操作参数(8)，与 0x06 读触发载荷
 *  同构（其后直接接标签页 4）。 */
#define NS2_NFC_WRITE_DESC_LEN 17u

/** 读取结束后的报告状态值（串口 `amiibo done <n>` 可调，扫「读取结束」的
 *  正确值用），默认 0x00 = Idle。 */
static uint8_t s_drained_state;

/** 读缓冲头区填充模式：1 = 结构模板（默认），0 = 全零（对账基线）；
 *  独立于 s_nfc，reset 不清。 */
static uint8_t s_header_mode = 1;

/** 抽块结束后的主动推送模式（ns2_nfc_set_push_mode）。 */
static uint8_t s_push_mode;

/** 待推的完成事件（0x15 EOF 时按模式生成，ble_session 补发）。 */
static ns2_nfc_event_t s_event;
static bool s_event_pending;

void ns2_nfc_reset(void)
{
  if (s_nfc.image != NULL) {
    heap_caps_free(s_nfc.image);
  }
  memset(&s_nfc, 0, sizeof(s_nfc));
  s_event_pending = false;
}

esp_err_t ns2_nfc_stage(const uint8_t *data, size_t len)
{
  if (data == NULL || len == 0) {
    if (s_nfc.image != NULL) {
      heap_caps_free(s_nfc.image);
      s_nfc.image = NULL;
      s_nfc.image_len = 0;
      memset(s_nfc.sig, 0, sizeof(s_nfc.sig));
      ESP_LOGI(TAG, "tag image cleared (nfc state -> idle)");
    }
    return ESP_OK;
  }
  if (len != NS2_NFC_TAG_SIZE && len != NS2_NFC_IMAGE_MAX) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (s_nfc.image == NULL) {
    s_nfc.image = heap_caps_malloc(NS2_NFC_TAG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_nfc.image == NULL) {
      s_nfc.image = heap_caps_malloc(NS2_NFC_TAG_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_nfc.image == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  memcpy(s_nfc.image, data, NS2_NFC_TAG_SIZE);
  s_nfc.image_len = NS2_NFC_TAG_SIZE;
  if (len == NS2_NFC_IMAGE_MAX) {
    memcpy(s_nfc.sig, &data[NS2_NFC_TAG_SIZE], NS2_NFC_SIG_SIZE);
  } else {
    memset(s_nfc.sig, 0, sizeof(s_nfc.sig));
  }
  ESP_LOGI(TAG, "tag staged: %u bytes sig=%u (polling=%u)", (unsigned)len, s_nfc.sig[0] != 0 || s_nfc.sig[31] != 0,
           s_nfc.polling);
  return ESP_OK;
}

bool ns2_nfc_ready(void)
{
  return s_nfc.image != NULL;
}

size_t ns2_nfc_read(uint32_t offset, uint8_t *out, size_t len)
{
  if (out == NULL || len == 0 || s_nfc.image == NULL || offset >= s_nfc.image_len) {
    return 0;
  }
  const size_t avail = s_nfc.image_len - offset;
  const size_t n = len < avail ? len : avail;
  memcpy(out, &s_nfc.image[offset], n);
  return n;
}

bool ns2_nfc_uid(uint8_t out[7])
{
  if (s_nfc.image == NULL || s_nfc.image_len < 9) {
    return false;
  }
  /* NTAG215 页 0-2：UID0-2 + BCC0 | UID3-5 + BCC1 | UID6 + 内部锁位。 */
  out[0] = s_nfc.image[0];
  out[1] = s_nfc.image[1];
  out[2] = s_nfc.image[2];
  out[3] = s_nfc.image[4];
  out[4] = s_nfc.image[5];
  out[5] = s_nfc.image[6];
  out[6] = s_nfc.image[8];
  return true;
}

void ns2_nfc_set_polling(bool on)
{
  if (s_nfc.polling != on) {
    s_nfc.polling = on;
    ESP_LOGI(TAG, "polling -> %u (nfc state 0x%02x)", on, ns2_nfc_report_state());
  }
}

bool ns2_nfc_polling(void)
{
  return s_nfc.polling;
}

/** 读卡流程的当前状态值：0 = 未在读书；0x14 = 读取中；超过模拟读卡耗时后按
 *  0x15（数据就绪）报告。 */
static uint8_t effective_stage(void)
{
  if (s_nfc.read_stage == 0x14u && esp_timer_get_time() - s_nfc.read_started_us >= NS2_NFC_STAGE_READY_DELAY_US) {
    return 0x15u;
  }
  return s_nfc.read_stage;
}

uint8_t ns2_nfc_report_state(void)
{
  /* 状态机（报告字节与 0x05 体首字节同源）：场开无卡 0x01；卡片在场 0x09；
     * 0x06 读卡触发后 0x14（读取中）→ 0x15（数据就绪）；主机抽完块（EOF 探测）
     * 报「读取结束」值（串口可调，默认 0x00 Idle）。 */
  if (!s_nfc.polling) {
    return 0x00u;
  }
  if (s_nfc.drained) {
    return s_drained_state;
  }
  const uint8_t stage = effective_stage();
  if (stage != 0x00u) {
    return stage;
  }
  return s_nfc.image != NULL ? 0x09u : 0x01u;
}

void ns2_nfc_set_drained_state(uint8_t state)
{
  s_drained_state = state;
  ESP_LOGI(TAG, "drained state -> 0x%02x", s_drained_state);
}

void ns2_nfc_set_header_mode(uint8_t mode)
{
  s_header_mode = mode ? 1u : 0u;
  ESP_LOGI(TAG, "read buffer header mode -> %u", s_header_mode);
}

uint8_t ns2_nfc_header_mode(void)
{
  return s_header_mode;
}

void ns2_nfc_set_push_mode(uint8_t mode)
{
  s_push_mode = mode;
  ESP_LOGI(TAG, "drain push mode -> %u", s_push_mode);
}

uint8_t ns2_nfc_push_mode(void)
{
  return s_push_mode;
}

bool ns2_nfc_pop_event(ns2_nfc_event_t *out)
{
  if (!s_event_pending || out == NULL) {
    return false;
  }
  *out = s_event;
  s_event_pending = false;
  return true;
}

/** 抽块结束时按推送模式生成完成事件（变体见 ns2_nfc.h）。 */
static void stage_drain_event(void)
{
  s_event_pending = false;
  if (s_push_mode == 0) {
    return;
  }
  uint8_t uid[7];
  if (!ns2_nfc_uid(uid)) {
    return;
  }
  memset(&s_event, 0, sizeof(s_event));
  s_event.body_len = 0;
  if (s_push_mode == 1 || s_push_mode == 2) {
    s_event.subcmd = 0x05;
    s_event.body_len = NS2_NFC_TAG_INFO_BODY_LEN;
    s_event.body[0] = 0x09;
    if (s_push_mode == 1) {
      /* MCU read3 形态：状态后跟 31 04 完成标记。 */
      s_event.body[1] = 0x31;
      s_event.body[2] = 0x04;
      s_event.body[6] = 0x01;
      s_event.body[7] = 0x01;
      s_event.body[8] = 0x02;
      s_event.body[10] = 0x07;
      memcpy(&s_event.body[11], uid, sizeof(uid));
    } else {
      s_event.body[4] = 0x01;
      s_event.body[5] = 0x01;
      s_event.body[6] = 0x02;
      s_event.body[8] = 0x07;
      memcpy(&s_event.body[9], uid, sizeof(uid));
    }
  } else if (s_push_mode == 3) {
    s_event.subcmd = 0x06;
  } else {
    s_event.subcmd = 0x15;
    s_event.body_len = 3;
  }
  s_event_pending = true;
}

void ns2_nfc_set_report_stage(uint8_t stage)
{
  s_nfc.read_stage = stage;
  s_nfc.read_started_us = esp_timer_get_time();
  s_nfc.drained = false;
  ESP_LOGI(TAG, "report stage -> 0x%02x (nfc state 0x%02x)", s_nfc.read_stage, ns2_nfc_report_state());
}

void ns2_nfc_set_write_sink(ns2_nfc_write_sink_fn fn, void *user)
{
  s_nfc.sink = fn;
  s_nfc.sink_user = user;
}

/** 0x05 取卡信息：63 字节体（前缀 + UID + 补零），感应区无卡时全零。
 *  体首字节 = 报告状态字节的同一状态源（在场 0x09，读卡流程中 0x14/0x15，
 *  读取结束 0x00）——两个通道的值必须一致，主机在读卡流程里交替观察它们。 */
static size_t build_tag_info(uint8_t *resp, size_t cap)
{
  if (cap < NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN) {
    return 0;
  }
  uint8_t *body = &resp[NS2_FRAME_HEADER_LEN];
  memset(body, 0, NS2_NFC_TAG_INFO_BODY_LEN);
  uint8_t uid[7];
  if (ns2_nfc_uid(uid)) {
    uint8_t prefix[sizeof(s_tag_info_prefix)];
    memcpy(prefix, s_tag_info_prefix, sizeof(prefix));
    prefix[0] = ns2_nfc_report_state();
    memcpy(body, prefix, sizeof(prefix));
    memcpy(&body[9], uid, sizeof(uid));
  }
  return NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN;
}

/** 读缓冲 60 字节头区：结构来自 Switch 1 MCU 时代的读卡响应（Poohl/joycontrol
 *  mcu.md read1 的标签数据前缀）；厂商签名取 572 字节 dump 尾部。 */
static void build_read_header(uint8_t *out)
{
  memset(out, 0, NS2_NFC_BUFFER_HEADER);
  if (!s_header_mode) {
    return;
  }
  uint8_t uid[7];
  if (!ns2_nfc_uid(uid)) {
    return;
  }
  out[0] = 0x31;
  out[1] = 0x02;
  out[5] = 0x01;
  out[6] = 0x02;
  out[7] = 0x00;
  out[8] = 0x07;
  memcpy(&out[9], uid, sizeof(uid));
  memcpy(&out[20], s_nfc.sig, NS2_NFC_SIG_SIZE);
  out[53] = 0x3B;
  out[54] = 0x3C;
  out[55] = 0x77;
  out[56] = 0x78;
  out[57] = 0x86;
}

/** 0x15 取读缓冲：`00`（状态）+ 数据长度（u16 LE）+ 最多 70 字节缓冲数据。
 *  缓冲区 = [60 字节读卡结果头][540 字节标签镜像]（NS2_NFC_BUFFER_HEADER/TOTAL）。 */
static size_t build_buffer_read(const uint8_t *req, size_t len, uint8_t *resp, size_t cap)
{
  if (len < NS2_FRAME_HEADER_LEN + 2 || cap < NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX) {
    return NS2_FRAME_HEADER_LEN;
  }
  const uint16_t offset = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));
  uint8_t *body = &resp[NS2_FRAME_HEADER_LEN];
  memset(&body[3], 0, NS2_NFC_READ_CHUNK_MAX);
  body[0] = 0x00;
  size_t n = 0;
  if (offset < NS2_NFC_BUFFER_TOTAL) {
    n = NS2_NFC_READ_CHUNK_MAX < NS2_NFC_BUFFER_TOTAL - offset ? NS2_NFC_READ_CHUNK_MAX : NS2_NFC_BUFFER_TOTAL - offset;
    if (offset < NS2_NFC_BUFFER_HEADER) {
      uint8_t header[NS2_NFC_BUFFER_HEADER];
      build_read_header(header);
      const size_t in_head = NS2_NFC_BUFFER_HEADER - offset < n ? NS2_NFC_BUFFER_HEADER - offset : n;
      const size_t from_tag = n - in_head;
      memcpy(&body[3], &header[offset], in_head);
      if (from_tag > 0) {
        (void)ns2_nfc_read(0, &body[3 + in_head], from_tag);
      }
    } else {
      (void)ns2_nfc_read(offset - NS2_NFC_BUFFER_HEADER, &body[3], n);
    }
  }
  body[1] = (uint8_t)(n & 0xFFu);
  body[2] = (uint8_t)(n >> 8);
  return NS2_FRAME_HEADER_LEN + 3 + n;
}

/** 0x14 装载写缓冲：`offset(u16 LE) + len(u16 LE) + 数据`；首次装载把镜像整份垫进
 *  缓冲（0xFF 补齐），保证部分写入提交后其余字节不变。首块若以 `d0 07` 操作描述符
 *  开头（描述符 17 字节后直接接标签页 4，UID/CC 只读页不写），进入流式模式：
 *  描述符剥掉、数据从页 4 起按序落位；不带描述符的装载保持按偏移直写。 */
static void load_write_buffer(const uint8_t *req, size_t len)
{
  if (len < NS2_FRAME_HEADER_LEN + 4) {
    return;
  }
  const uint16_t offset = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));
  const uint16_t data_len = (uint16_t)(req[10] | ((uint16_t)req[11] << 8));
  if ((size_t)(NS2_FRAME_HEADER_LEN + 4 + data_len) > len) {
    ESP_LOGW(TAG, "write buffer load rejected: off=%u len=%u", offset, data_len);
    return;
  }
  if (!s_nfc.write_dirty) {
    memset(s_nfc.write_buf, 0xFF, sizeof(s_nfc.write_buf));
    if (s_nfc.image != NULL) {
      memcpy(s_nfc.write_buf, s_nfc.image, s_nfc.image_len);
    }
    s_nfc.write_stream = false;
    s_nfc.write_pos = 0;
  }
  const uint8_t *data = &req[NS2_FRAME_HEADER_LEN + 4];
  size_t n = data_len;
  if (offset == 0 && !s_nfc.write_stream && n >= NS2_NFC_WRITE_DESC_LEN && data[0] == 0xD0 && data[1] == 0x07) {
    data += NS2_NFC_WRITE_DESC_LEN;
    n -= NS2_NFC_WRITE_DESC_LEN;
    s_nfc.write_stream = true;
    s_nfc.write_pos = 16; /* 标签页 4 起步：页 0-3（UID/CC）只读不写。 */
  }
  if (s_nfc.write_stream) {
    if (s_nfc.write_pos + n > NS2_NFC_TAG_SIZE) {
      ESP_LOGW(TAG, "write stream overflow: pos=%u n=%u", (unsigned)s_nfc.write_pos, (unsigned)n);
      return;
    }
    memcpy(&s_nfc.write_buf[s_nfc.write_pos], data, n);
    s_nfc.write_pos += n;
  } else {
    if (offset >= NS2_NFC_TAG_SIZE || (size_t)offset + n > NS2_NFC_TAG_SIZE) {
      ESP_LOGW(TAG, "write buffer load rejected: off=%u len=%u", offset, data_len);
      return;
    }
    memcpy(&s_nfc.write_buf[offset], data, n);
  }
  s_nfc.write_dirty = true;
  ESP_LOGI(TAG, "write buffer +%u bytes (%s, stream pos=%u)", data_len,
           s_nfc.write_stream ? "descriptor stream" : "raw offset", (unsigned)s_nfc.write_pos);
}

/** 0x08 提交写卡：写缓冲整体落进镜像，并经写回回调交给存储层落盘。 */
static void commit_write_buffer(void)
{
  if (!s_nfc.write_dirty) {
    return;
  }
  if (s_nfc.image == NULL && ns2_nfc_stage(s_nfc.write_buf, NS2_NFC_TAG_SIZE) != ESP_OK) {
    ESP_LOGE(TAG, "commit failed: no image buffer");
    return;
  }
  memcpy(s_nfc.image, s_nfc.write_buf, NS2_NFC_TAG_SIZE);
  s_nfc.image_len = NS2_NFC_TAG_SIZE;
  s_nfc.write_dirty = false;
  bool stored = false;
  if (s_nfc.sink != NULL) {
    stored = s_nfc.sink(s_nfc.image, NS2_NFC_TAG_SIZE, s_nfc.sink_user);
  }
  ESP_LOGI(TAG, "write committed (540 bytes, stored=%u)", stored);
}

bool ns2_nfc_response_ack(uint8_t subcmd, uint8_t *status, uint8_t *ack)
{
  switch (subcmd) {
  case 0x0C:
  case 0x15:
    *status = 0x10;
    *ack = 0x78;
    return true;
  case 0x03:
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x08:
  case 0x14:
    *status = 0x00;
    *ack = 0xF8;
    return true;
  default:
    return false;
  }
}

size_t ns2_nfc_on_command(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp, size_t cap)
{
  switch (subcmd) {
  case 0x03:
    /* 开射频场轮询：5 字节参数（如 00 E8 03 2C 01）语义未公开，不参与决策；
         * 重新开轮询同时复位读卡流程（状态字节回到卡片在场 0x09）。 */
    s_nfc.polling = true;
    s_nfc.read_stage = 0;
    s_nfc.drained = false;
    ESP_LOGI(TAG, "host started polling (nfc state 0x%02x)", ns2_nfc_report_state());
    return NS2_FRAME_HEADER_LEN;
  case 0x04:
    s_nfc.polling = false;
    s_nfc.read_stage = 0;
    s_nfc.drained = false;
    ESP_LOGI(TAG, "host stopped polling");
    return NS2_FRAME_HEADER_LEN;
  case 0x05: {
    const size_t built = build_tag_info(resp, cap);
    uint8_t uid[7];
    if (ns2_nfc_uid(uid)) {
      ESP_LOGI(TAG, "tag info -> uid %02x%02x%02x%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5],
               uid[6]);
    } else {
      ESP_LOGI(TAG, "tag info -> empty (no image staged)");
    }
    return built;
  }
  case 0x06:
    /* 触发读卡：镜像常驻内存，读卡即刻成立。状态按 0x14（读取中）→ 0x15（数据就绪）
         * 跃迁——主机在 0x06 后检查一次 0x05 体首字节、随后等报告状态字节到 0x15 才拉取。 */
    s_nfc.read_stage = 0x14u;
    s_nfc.drained = false;
    s_nfc.read_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "host read request (tag %s) -> nfc state 0x%02x", s_nfc.image != NULL ? "present" : "absent",
             ns2_nfc_report_state());
    return NS2_FRAME_HEADER_LEN;
  case 0x08:
    commit_write_buffer();
    return NS2_FRAME_HEADER_LEN;
  case 0x0C:
    if (cap < NS2_FRAME_HEADER_LEN + sizeof(s_nfc_status)) {
      return NS2_FRAME_HEADER_LEN;
    }
    memcpy(&resp[NS2_FRAME_HEADER_LEN], s_nfc_status, sizeof(s_nfc_status));
    return NS2_FRAME_HEADER_LEN + sizeof(s_nfc_status);
  case 0x14:
    load_write_buffer(req, len);
    return NS2_FRAME_HEADER_LEN;
  case 0x15: {
    /* 主机抽块期间状态保持在 0x15（数据就绪）不变：每拉一块就翻回 0x14
         * 会让主机看到「就绪状态消失」而中途放弃抽块，保持 0x15 才能连续抽完。
         * 主机拉到缓冲区末端（应答长度 0）即整份读完毕，状态报「读取结束」
         * 值（串口可调）交给上层。 */
    const uint16_t offset = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));
    const size_t built = build_buffer_read(req, len, resp, cap);
    if (offset >= NS2_NFC_BUFFER_TOTAL) {
      s_nfc.read_stage = 0;
      s_nfc.drained = true;
      stage_drain_event();
      ESP_LOGI(TAG, "read drained (nfc state -> 0x%02x, push=%u)", s_drained_state, s_push_mode);
    }
    return built;
  }
  default:
    ESP_LOGW(TAG, "nfc subcmd 0x%02x unsupported", subcmd);
    return NS2_FRAME_HEADER_LEN;
  }
}

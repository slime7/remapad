#include "dp_capture.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

/** 队列与计数由 NimBLE 主机任务（写）与数据面任务（读）共享，短临界区保护。 */
static portMUX_TYPE s_capture_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
  bool enabled;
  dp_capture_record_t queue[DP_CAPTURE_QUEUE_LEN];
  size_t head;  /* 最老记录的下标 */
  size_t count; /* 排队记录数 */
  uint8_t seq;  /* 下一条记录的记录号 */
  uint32_t pushed;
  uint32_t dropped;
} s_capture;

void dp_capture_set_enabled(bool on)
{
  portENTER_CRITICAL(&s_capture_mux);
  s_capture.enabled = on;
  s_capture.head = 0;
  s_capture.count = 0;
  s_capture.seq = 0;
  s_capture.pushed = 0;
  s_capture.dropped = 0;
  portEXIT_CRITICAL(&s_capture_mux);
}

bool dp_capture_enabled(void)
{
  portENTER_CRITICAL(&s_capture_mux);
  const bool enabled = s_capture.enabled;
  portEXIT_CRITICAL(&s_capture_mux);
  return enabled;
}

void dp_capture_host_write(uint8_t channel, const uint8_t *data, size_t len)
{
  portENTER_CRITICAL(&s_capture_mux);
  if (!s_capture.enabled) {
    portEXIT_CRITICAL(&s_capture_mux);
    return;
  }
  if (data == NULL || len == 0) {
    /* 空写入不进队列也不计丢包：dropped 只回答「队列满丢了多少」。 */
    portEXIT_CRITICAL(&s_capture_mux);
    return;
  }
  if (s_capture.count >= DP_CAPTURE_QUEUE_LEN) {
    s_capture.dropped++;
    portEXIT_CRITICAL(&s_capture_mux);
    return;
  }
  dp_capture_record_t *rec = &s_capture.queue[(s_capture.head + s_capture.count) % DP_CAPTURE_QUEUE_LEN];
  rec->channel = channel;
  rec->truncated = len > DP_CAPTURE_MAX_DATA;
  rec->len = rec->truncated ? DP_CAPTURE_MAX_DATA : (uint8_t)len;
  rec->seq = s_capture.seq++;
  memcpy(rec->data, data, rec->len);
  s_capture.count++;
  s_capture.pushed++;
  portEXIT_CRITICAL(&s_capture_mux);
}

size_t dp_capture_pop_payload(uint8_t *out, size_t cap, uint8_t *slot)
{
  if (out == NULL || cap < 2u) {
    return 0;
  }
  size_t payload_len = 0;
  portENTER_CRITICAL(&s_capture_mux);
  if (s_capture.count > 0) {
    dp_capture_record_t *rec = &s_capture.queue[s_capture.head];
    const size_t need = 2u + rec->len;
    if (cap >= need) {
      out[0] = rec->channel;
      out[1] = (uint8_t)(rec->len | (rec->truncated ? 0x80u : 0u));
      if (rec->len > 0) {
        memcpy(&out[2], rec->data, rec->len);
      }
      if (slot != NULL) {
        *slot = rec->seq;
      }
      payload_len = need;
      /* 编码成功才出队：缓冲不足时记录留在队首，等调用方给足缓冲。 */
      s_capture.head = (s_capture.head + 1) % DP_CAPTURE_QUEUE_LEN;
      s_capture.count--;
    }
  }
  portEXIT_CRITICAL(&s_capture_mux);
  return payload_len;
}

void dp_capture_counts(uint32_t *pushed, uint32_t *dropped)
{
  portENTER_CRITICAL(&s_capture_mux);
  if (pushed != NULL) {
    *pushed = s_capture.pushed;
  }
  if (dropped != NULL) {
    *dropped = s_capture.dropped;
  }
  portEXIT_CRITICAL(&s_capture_mux);
}

#include "ota_proto.h"

#include <string.h>

static ota_proto_result_t result_from(const ota_proto_t *proto, bool reply)
{
  const ota_proto_result_t result = {
    .state = proto->state,
    .code = proto->code,
    .next_seq = proto->next_seq,
    .received = proto->received,
    .reply = reply,
    .finished = false,
  };
  return result;
}

/** 作废会话：状态转失败并回一个错误码，聚合缓冲里的残留字节丢弃。 */
static ota_proto_result_t fail(ota_proto_t *proto, ota_code_t code)
{
  proto->state = OTA_STATE_FAILED;
  proto->code = code;
  proto->chunk_len = 0;
  proto->finished = false;
  return result_from(proto, true);
}

void ota_proto_init(ota_proto_t *proto)
{
  if (proto == NULL) {
    return;
  }
  memset(proto, 0, sizeof(*proto));
  proto->state = OTA_STATE_IDLE;
  proto->code = OTA_CODE_OK;
}

bool ota_proto_parse_begin(const uint8_t *payload, size_t len, uint32_t *image_size)
{
  if (payload == NULL || image_size == NULL || len < OTA_BEGIN_PAYLOAD_LEN) {
    return false;
  }
  if (memcmp(payload, OTA_BEGIN_MAGIC, OTA_BEGIN_MAGIC_LEN) != 0) {
    return false;
  }
  const uint8_t *size = &payload[OTA_BEGIN_MAGIC_LEN];
  *image_size = (uint32_t)size[0] | ((uint32_t)size[1] << 8) | ((uint32_t)size[2] << 16) | ((uint32_t)size[3] << 24);
  return true;
}

ota_proto_result_t ota_proto_begin(ota_proto_t *proto, uint32_t image_size, uint32_t max_image_size, int64_t now_us)
{
  /* 接收中或刚收完（等待校验）时拒绝新的 BEGIN：旧会话要么超时收尾、要么
     * 走完重启，PC 端据 BUSY 决定重试时机。 */
  if (proto->state == OTA_STATE_RECEIVING || proto->state == OTA_STATE_DONE) {
    proto->code = OTA_CODE_BUSY;
    return result_from(proto, true);
  }
  if (image_size == 0 || image_size > max_image_size) {
    return fail(proto, OTA_CODE_BAD_HEADER);
  }
  proto->state = OTA_STATE_RECEIVING;
  proto->code = OTA_CODE_OK;
  proto->next_seq = 0;
  proto->received = 0;
  proto->image_size = image_size;
  proto->chunk_len = 0;
  proto->accepted_since_ack = 0;
  proto->seq_error_reported = false;
  proto->seq_error_reply_us = 0;
  proto->finished = false;
  proto->last_rx_us = now_us;
  return result_from(proto, true);
}

void ota_proto_note_rx(ota_proto_t *proto, int64_t now_us)
{
  if (proto != NULL && proto->state == OTA_STATE_RECEIVING) {
    proto->last_rx_us = now_us;
  }
}

ota_proto_result_t ota_proto_data(ota_proto_t *proto, const uint8_t *payload, size_t len, bool window_end,
                                  int64_t now_us, ota_flush_fn flush, void *user)
{
  if (proto->state != OTA_STATE_RECEIVING || proto->finished || payload == NULL) {
    proto->code = OTA_CODE_BAD_HEADER;
    return result_from(proto, true);
  }
  proto->last_rx_us = now_us;
  if (len < OTA_DATA_SEQ_LEN) {
    proto->code = OTA_CODE_SEQ_ERROR;
    return result_from(proto, true);
  }
  const uint16_t seq = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
  if (seq != proto->next_seq) {
    /* 重发或丢帧：回当前期望序号，PC 从这里续传、不重复写 flash。整窗重发时
         * 所有重发帧都落在同一个期望序号上，按最小间隔限流，避免一串应答淹掉后续；
         * 限流不撤销已经发过的那次，PC 隔一会儿重问仍然拿得到应答。 */
    proto->code = OTA_CODE_SEQ_ERROR;
    const bool report =
        !proto->seq_error_reported || now_us - proto->seq_error_reply_us >= OTA_DUPLICATE_REPLY_MIN_INTERVAL_US;
    if (report) {
      proto->seq_error_reported = true;
      proto->seq_error_reply_us = now_us;
    }
    return result_from(proto, report);
  }
  const size_t data_len = len - OTA_DATA_SEQ_LEN;
  if ((uint64_t)proto->received + data_len > proto->image_size) {
    return fail(proto, OTA_CODE_SIZE_MISMATCH);
  }
  size_t copied = 0;
  while (copied < data_len) {
    const size_t room = OTA_CHUNK_LEN - proto->chunk_len;
    const size_t take = (data_len - copied) < room ? (data_len - copied) : room;
    memcpy(&proto->chunk[proto->chunk_len], &payload[OTA_DATA_SEQ_LEN + copied], take);
    proto->chunk_len += take;
    copied += take;
    if (proto->chunk_len == OTA_CHUNK_LEN) {
      const ota_code_t code = flush != NULL ? flush(proto->chunk, proto->chunk_len, user) : OTA_CODE_OK;
      proto->chunk_len = 0;
      if (code != OTA_CODE_OK) {
        return fail(proto, code);
      }
    }
  }
  proto->received += (uint32_t)data_len;
  proto->next_seq = (uint16_t)(proto->next_seq + 1u);
  proto->code = OTA_CODE_OK;
  proto->seq_error_reported = false;
  proto->seq_error_reply_us = 0;
  proto->accepted_since_ack++;
  if (window_end || proto->accepted_since_ack >= OTA_ACK_WINDOW) {
    proto->accepted_since_ack = 0;
    return result_from(proto, true);
  }
  return result_from(proto, false);
}

ota_proto_result_t ota_proto_end(ota_proto_t *proto, int64_t now_us, ota_flush_fn flush, void *user)
{
  if (proto->state != OTA_STATE_RECEIVING || proto->finished) {
    proto->code = OTA_CODE_BAD_HEADER;
    return result_from(proto, true);
  }
  proto->last_rx_us = now_us;
  if (proto->received != proto->image_size) {
    return fail(proto, OTA_CODE_SIZE_MISMATCH);
  }
  if (proto->chunk_len > 0) {
    const ota_code_t code = flush != NULL ? flush(proto->chunk, proto->chunk_len, user) : OTA_CODE_OK;
    proto->chunk_len = 0;
    if (code != OTA_CODE_OK) {
      return fail(proto, code);
    }
  }
  proto->finished = true;
  proto->code = OTA_CODE_OK;
  ota_proto_result_t result = result_from(proto, false);
  result.finished = true;
  return result;
}

ota_proto_result_t ota_proto_tick(ota_proto_t *proto, int64_t now_us)
{
  if (proto->state != OTA_STATE_RECEIVING || proto->finished) {
    return result_from(proto, false);
  }
  if (now_us - proto->last_rx_us > OTA_SESSION_TIMEOUT_US) {
    return fail(proto, OTA_CODE_TIMEOUT);
  }
  return result_from(proto, false);
}

ota_proto_result_t ota_proto_fail(ota_proto_t *proto, ota_code_t code)
{
  return fail(proto, code);
}

ota_proto_result_t ota_proto_done(ota_proto_t *proto)
{
  proto->state = OTA_STATE_DONE;
  proto->code = OTA_CODE_OK;
  proto->chunk_len = 0;
  proto->finished = false;
  return result_from(proto, true);
}

bool ota_proto_busy(const ota_proto_t *proto)
{
  return proto->state == OTA_STATE_RECEIVING || proto->state == OTA_STATE_DONE;
}

size_t ota_proto_encode_ack(const ota_proto_result_t *result, const char *version, bool with_version, uint8_t *out,
                            size_t out_len)
{
  if (result == NULL || out == NULL) {
    return 0;
  }
  const size_t total = OTA_ACK_PAYLOAD_LEN + (with_version ? OTA_ACK_VERSION_LEN : 0u);
  if (out_len < total) {
    return 0;
  }
  out[0] = (uint8_t)result->state;
  out[1] = (uint8_t)result->code;
  out[2] = (uint8_t)(result->next_seq & 0xFFu);
  out[3] = (uint8_t)(result->next_seq >> 8);
  out[4] = (uint8_t)(result->received & 0xFFu);
  out[5] = (uint8_t)((result->received >> 8) & 0xFFu);
  out[6] = (uint8_t)((result->received >> 16) & 0xFFu);
  out[7] = (uint8_t)((result->received >> 24) & 0xFFu);
  if (with_version) {
    memset(&out[OTA_ACK_PAYLOAD_LEN], 0, OTA_ACK_VERSION_LEN);
    if (version != NULL) {
      size_t len = strlen(version);
      if (len > OTA_ACK_VERSION_LEN) {
        len = OTA_ACK_VERSION_LEN;
      }
      memcpy(&out[OTA_ACK_PAYLOAD_LEN], version, len);
    }
  }
  return total;
}

#include "amiibo_session.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "amiibo_proto.h"
#include "amiibo_store.h"
#include "input_link.h"

static const char *TAG = "remapad_amiibo";

/** 应答等发送环的上限：串口日志刷屏时环会满，但绝不在接收任务里无限等。 */
#define AMIIBO_ACK_TX_TIMEOUT_MS 200u

static amiibo_upload_t s_up;

bool amiibo_session_is_frame_type(uint8_t type)
{
  return type >= 0x40 && type <= 0x43;
}

/** 按 proto 结论回一帧 ACK。 */
static void reply(const amiibo_upload_result_t *result)
{
  if (!result->reply) {
    return;
  }
  uint8_t payload[AMIIBO_ACK_PAYLOAD_LEN];
  const size_t len = amiibo_upload_encode_ack(result, payload, sizeof(payload));
  if (len == 0) {
    return;
  }
  input_link_send_frame_wait(INPUT_FRAME_TYPE_AMIIBO_ACK, 0, payload, len, AMIIBO_ACK_TX_TIMEOUT_MS);
}

void amiibo_session_handle_frame(const input_frame_view_t *frame)
{
  const int64_t now = esp_timer_get_time();
  /* PC 端断线或被杀后重开上传：超时的旧会话在这里顺带清理（无独立任务）。 */
  const amiibo_upload_result_t stale = amiibo_upload_tick(&s_up, now);
  if (stale.reply) {
    ESP_LOGW(TAG, "upload timed out, session reset");
    reply(&stale);
  }

  amiibo_upload_result_t result = {
    .state = s_up.state,
    .code = AMIIBO_CODE_OK,
    .received = s_up.received,
    .slot = -1,
    .reply = false,
    .finished = false,
  };
  switch (frame->type) {
  case INPUT_FRAME_TYPE_AMIIBO_BEGIN:
    result = amiibo_upload_begin(&s_up, frame->payload, frame->payload_len, now);
    if (result.state == AMIIBO_STATE_RECEIVING) {
      ESP_LOGI(TAG, "upload '%s' started", s_up.name);
    } else {
      ESP_LOGW(TAG, "upload begin refused (code=%d)", (int)result.code);
    }
    break;
  case INPUT_FRAME_TYPE_AMIIBO_DATA:
    result = amiibo_upload_data(&s_up, frame->payload, frame->payload_len, now);
    if (result.code != AMIIBO_CODE_OK) {
      ESP_LOGW(TAG, "upload data refused (code=%d, received=%u/%u)", (int)result.code, (unsigned)result.received,
               (unsigned)NS2_NFC_TAG_SIZE);
    }
    break;
  case INPUT_FRAME_TYPE_AMIIBO_END:
    result = amiibo_upload_end(&s_up, now, amiibo_store_add, NULL);
    if (result.finished) {
      ESP_LOGI(TAG, "upload done: '%s' -> slot %d", s_up.name, result.slot);
    } else if (result.code != AMIIBO_CODE_OK) {
      ESP_LOGW(TAG, "upload end failed (code=%d)", (int)result.code);
    }
    break;
  default:
    ESP_LOGW(TAG, "amiibo frame 0x%02x ignored", frame->type);
    return;
  }
  reply(&result);
}

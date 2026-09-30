#include "ota_link.h"

#include <stdio.h>

#include "esp_log.h"

#include "bridge/js_bridge.h"
#include "input_link.h"
#include "ota_proto.h"
#include "ota_session.h"

static const char *TAG = "remapad_ota";

/** ACK 等发送环的上限：串口日志会挤占发送环，升级应答不能像数据面那样随手丢。 */
#define OTA_LINK_ACK_TX_TIMEOUT_MS 200u

static const char *phase_name(ota_session_phase_t phase)
{
  switch (phase) {
  case OTA_SESSION_PHASE_RECEIVING:
    return "receiving";
  case OTA_SESSION_PHASE_VERIFYING:
    return "verifying";
  case OTA_SESSION_PHASE_REBOOTING:
    return "rebooting";
  case OTA_SESSION_PHASE_FAILED:
    return "failed";
  default:
    return "idle";
  }
}

/** 阶段/进度以 JSON 事件进桥接事件队列，由 owner task 回发界面；任意任务上下文可调。 */
static void post_progress(ota_session_phase_t phase, uint32_t received, uint32_t total, void *user)
{
  (void)user;
  char event[128];
  const uint32_t pct = total > 0u ? (uint32_t)((uint64_t)received * 100u / total) : 0u;
  const int len = snprintf(event, sizeof(event),
                           "{\"t\":\"otaProgress\",\"phase\":\"%s\",\"received\":%u,"
                           "\"total\":%u,\"percentage\":%u}",
                           phase_name(phase), (unsigned)received, (unsigned)total, (unsigned)pct);
  if (len > 0 && (size_t)len < sizeof(event)) {
    js_bridge_post_event(event);
  }
}

/** ACK 双路出口：OTA 帧从串口或 WiFi 进来共用一个会话核心，应答按在位的通道
 *  走——UDP 桥接在位投 UDP，串口已连接走 USJ，两边都在就都发（重复应答对
 *  发送端幂等；少发才让发送端干等）。两条路都不通返回错误，会话照常推进。 */
static esp_err_t send_ack(const uint8_t *payload, size_t len, void *user)
{
  (void)user;
  bool sent = false;
  if (input_link_send_frame_net(INPUT_FRAME_TYPE_OTA_ACK, 0, payload, len)) {
    sent = true;
  }
  if (input_link_pc_connected()) {
    const esp_err_t serial_err =
        input_link_send_frame_wait(INPUT_FRAME_TYPE_OTA_ACK, 0, payload, len, OTA_LINK_ACK_TX_TIMEOUT_MS);
    if (serial_err == ESP_OK) {
      sent = true;
    }
  }
  return sent ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool ota_link_is_frame_type(uint8_t type)
{
  return type == INPUT_FRAME_TYPE_OTA_BEGIN || type == INPUT_FRAME_TYPE_OTA_DATA || type == INPUT_FRAME_TYPE_OTA_END;
}

void ota_link_handle_frame(const input_frame_view_t *frame)
{
  if (frame == NULL) {
    return;
  }
  ota_session_msg_t msg = OTA_SESSION_MSG_END;
  if (frame->type == INPUT_FRAME_TYPE_OTA_BEGIN) {
    msg = OTA_SESSION_MSG_BEGIN;
  } else if (frame->type == INPUT_FRAME_TYPE_OTA_DATA) {
    msg = OTA_SESSION_MSG_DATA;
  }
  ota_session_handle_frame(msg, frame->slot == OTA_SLOT_WINDOW_END, frame->payload, frame->payload_len);
}

esp_err_t ota_link_start(void)
{
  static const ota_session_port_t port = {
    .send_ack = send_ack,
    .on_progress = post_progress,
    .user = NULL,
  };
  const esp_err_t err = ota_session_start(&port);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "ota link ready (frames 0x30-0x32 on serial / wifi)");
  }
  return err;
}

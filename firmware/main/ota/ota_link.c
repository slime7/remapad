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

/** 最近进帧的通道：单发送端场景下 ACK 就该发回去的那条路。串口与 netlog 收
 *  任务都会写，bool 写在 Xtensa 上天然原子；升级中只有一个发送端，不追多端竞争。 */
static volatile bool s_ack_via_net;

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

/** ACK 单路出口：跟最近进帧的通道走——对端在哪个口发升级帧就在哪个口收应答。
 *  之前两条路都发：WiFi 升级时串口线插着但没人打开 COM 口，每条应答都在 USJ
 *  发送环上白等满超时，往返延迟被抬高到把 PC 的窗口超时打穿。出口不在位时
 *  丢弃（PC 端按窗口超时重发兜住），会话照常推进。 */
static esp_err_t send_ack(const uint8_t *payload, size_t len, void *user)
{
  (void)user;
  if (s_ack_via_net) {
    if (!input_link_send_frame_net(INPUT_FRAME_TYPE_OTA_ACK, 0, payload, len)) {
      return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
  }
  if (!input_link_pc_connected()) {
    return ESP_ERR_INVALID_STATE;
  }
  return input_link_send_frame_wait(INPUT_FRAME_TYPE_OTA_ACK, 0, payload, len, OTA_LINK_ACK_TX_TIMEOUT_MS);
}

bool ota_link_is_frame_type(uint8_t type)
{
  return type == INPUT_FRAME_TYPE_OTA_BEGIN || type == INPUT_FRAME_TYPE_OTA_DATA || type == INPUT_FRAME_TYPE_OTA_END;
}

void ota_link_handle_frame(const input_frame_view_t *frame, bool from_net)
{
  if (frame == NULL) {
    return;
  }
  s_ack_via_net = from_net;
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

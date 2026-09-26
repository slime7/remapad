#include "ota_session.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "input_link.h"
#include "bridge/js_bridge.h"
#include "ota_proto.h"

static const char *TAG = "remapad_ota";

/** 帧队列按 PC 端一个窗口 16 帧设计：接收任务只入队，绝不阻塞。 */
#define OTA_QUEUE_LEN 16u
/** 任务栈来自内部 RAM（xTaskCreate 默认），flash 写入的禁缓存窗口离不开它。 */
#define OTA_TASK_STACK 6144u
/** 与接收任务同级：两个任务按时间片轮流推进，升级期间不掉字节。 */
#define OTA_TASK_PRIO 6
/** 队列轮询周期，同时充当超时检查与健康门槛检查的心跳。 */
#define OTA_POLL_MS 200u
/** 完成后先让 USJ 把应答帧送出去，再重启。 */
#define OTA_REBOOT_DELAY_MS 500u
/** 应答等发送环的上限：串口日志会挤占发送环，升级应答不能像数据面那样随手丢。 */
#define OTA_ACK_TX_TIMEOUT_MS 200u
/** 健康门槛：UI 首帧成功且开机满这么久，才确认新镜像有效。 */
#define OTA_HEALTH_MIN_UPTIME_US (30 * 1000 * 1000LL)

typedef enum {
  OTA_PHASE_IDLE = 0,
  OTA_PHASE_RECEIVING,
  OTA_PHASE_VERIFYING,
  OTA_PHASE_REBOOTING,
  OTA_PHASE_FAILED,
} ota_phase_t;

/** 队列槽：载荷最大的是 DATA 帧（序号 + 200 字节数据）。 */
typedef struct {
  uint8_t type;
  /** 该数据帧带窗口末帧标记（帧 slot 字段），收到即回应答。 */
  bool window_end;
  uint16_t len;
  uint8_t data[OTA_DATA_PAYLOAD_MAX];
} ota_queue_slot_t;

static struct {
  QueueHandle_t queue;
  TaskHandle_t task;
  ota_proto_t proto;
  esp_ota_handle_t handle;
  const esp_partition_t *target;
  bool handle_open;
  volatile bool ui_ready;
  bool health_confirmed;
  ota_phase_t phase;
  /** 上次向 UI 广播的整数百分比：数据帧按 1% 粒度限频，不逐帧刷事件队列。 */
  uint32_t reported_pct;
} s_ota;

static const char *phase_name(ota_phase_t phase)
{
  switch (phase) {
  case OTA_PHASE_RECEIVING:
    return "receiving";
  case OTA_PHASE_VERIFYING:
    return "verifying";
  case OTA_PHASE_REBOOTING:
    return "rebooting";
  case OTA_PHASE_FAILED:
    return "failed";
  default:
    return "idle";
  }
}

/** 向 UI 广播 OTA 进度：事件经外部队列由 owner task 回发，任意任务上下文可调。 */
static void notify_ui(ota_phase_t phase, uint32_t received, uint32_t total)
{
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

/** 接收进度按整数百分比限频广播：同一百分比的数据帧不重复进事件队列。 */
static void notify_receiving(uint32_t received, uint32_t total)
{
  const uint32_t pct = total > 0u ? (uint32_t)((uint64_t)received * 100u / total) : 0u;
  if (pct == s_ota.reported_pct) {
    return;
  }
  s_ota.reported_pct = pct;
  notify_ui(OTA_PHASE_RECEIVING, received, total);
}

const char *ota_session_state_name(void)
{
  return phase_name(s_ota.phase);
}

void ota_session_progress(int *phase, int *percent)
{
  const uint32_t total = s_ota.proto.image_size;
  const uint32_t received = s_ota.proto.received;
  if (phase != NULL) {
    *phase = (int)s_ota.phase;
  }
  if (percent != NULL) {
    *percent = total > 0U ? (int)((uint64_t)received * 100U / total) : 0;
  }
}

const char *ota_session_running_version(void)
{
  const esp_app_desc_t *desc = esp_app_get_description();
  return desc != NULL ? desc->version : "unknown";
}

const char *ota_session_running_partition(void)
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running != NULL ? running->label : "?";
}

bool ota_session_pending_verify(void)
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return false;
  }
  return state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_session_rollback_and_reboot(void)
{
  if (!ota_session_pending_verify()) {
    return ESP_ERR_INVALID_STATE;
  }
  return esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool ota_session_is_frame_type(uint8_t type)
{
  return type == INPUT_FRAME_TYPE_OTA_BEGIN || type == INPUT_FRAME_TYPE_OTA_DATA || type == INPUT_FRAME_TYPE_OTA_END;
}

/** 回一帧 ACK；BEGIN 的应答带 16 字节运行版本，便于 PC 端显示升级方向。 */
static void reply(const ota_proto_result_t *result, bool with_version)
{
  uint8_t payload[OTA_ACK_PAYLOAD_LEN + OTA_ACK_VERSION_LEN];
  const size_t len =
      ota_proto_encode_ack(result, ota_session_running_version(), with_version, payload, sizeof(payload));
  if (len == 0) {
    return;
  }
  input_link_send_frame_wait(INPUT_FRAME_TYPE_OTA_ACK, 0, payload, len, OTA_ACK_TX_TIMEOUT_MS);
}

/** 交一块聚合好的镜像数据给 flash：缓冲与栈都在内部 RAM，可在禁缓存窗口内读。 */
static ota_code_t ota_flash_write(const uint8_t *data, size_t len, void *user)
{
  (void)user;
  if (!s_ota.handle_open) {
    return OTA_CODE_FLASH_ERROR;
  }
  const esp_err_t err = esp_ota_write(s_ota.handle, data, len);
  if (err == ESP_OK) {
    return OTA_CODE_OK;
  }
  ESP_LOGE(TAG, "esp_ota_write failed: %s (len=%u)", esp_err_to_name(err), (unsigned)len);
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
    /* 首字节 magic 不对：送来的不是本工程的 ESP32 应用镜像。 */
    return OTA_CODE_BAD_HEADER;
  }
  return OTA_CODE_FLASH_ERROR;
}

/** 关掉 flash 会话；已写入的数据不作废，启动分区保持原样。 */
static void abort_flash_session(void)
{
  if (s_ota.handle_open) {
    esp_ota_abort(s_ota.handle);
    s_ota.handle_open = false;
  }
  s_ota.target = NULL;
}

static void handle_begin(const uint8_t *payload, size_t len)
{
  const int64_t now = esp_timer_get_time();
  uint32_t image_size = 0;
  if (!ota_proto_parse_begin(payload, len, &image_size)) {
    const ota_proto_result_t bad = ota_proto_fail(&s_ota.proto, OTA_CODE_BAD_HEADER);
    reply(&bad, true);
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, 0, 0);
    ESP_LOGW(TAG, "ota begin rejected: bad header");
    return;
  }
  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  if (target == NULL) {
    const ota_proto_result_t result = ota_proto_fail(&s_ota.proto, OTA_CODE_BUSY);
    reply(&result, true);
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, 0, 0);
    ESP_LOGE(TAG, "no update partition available");
    return;
  }
  /* 会话占用中（接收中或刚收完等校验）或尺寸越界都在这里被挡下，不碰 flash。 */
  ota_proto_result_t result = ota_proto_begin(&s_ota.proto, image_size, target->size, now);
  if (result.state != OTA_STATE_RECEIVING) {
    reply(&result, true);
    if (result.code != OTA_CODE_BUSY) {
      s_ota.phase = OTA_PHASE_FAILED;
      notify_ui(OTA_PHASE_FAILED, 0, image_size);
    }
    ESP_LOGW(TAG, "ota begin refused (code=%d, image=%u, partition=%s %u bytes)", (int)result.code,
             (unsigned)image_size, target->label, (unsigned)target->size);
    return;
  }

  esp_ota_handle_t handle = 0;
  /* esp_ota_begin 会按声明大小预擦目标分区，耗时可达数秒，应答落在这之后。 */
  const esp_err_t err = esp_ota_begin(target, image_size, &handle);
  if (err != ESP_OK) {
    const ota_code_t code = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? OTA_CODE_BUSY : OTA_CODE_FLASH_ERROR;
    result = ota_proto_fail(&s_ota.proto, code);
    reply(&result, true);
    if (code != OTA_CODE_BUSY) {
      s_ota.phase = OTA_PHASE_FAILED;
      notify_ui(OTA_PHASE_FAILED, 0, image_size);
    }
    ESP_LOGE(TAG, "esp_ota_begin failed: %s (image=%u)", esp_err_to_name(err), (unsigned)image_size);
    return;
  }
  s_ota.handle = handle;
  s_ota.handle_open = true;
  s_ota.target = target;
  s_ota.phase = OTA_PHASE_RECEIVING;
  s_ota.reported_pct = 0;
  /* 空闲超时从应答时刻起算：预擦已经过去，接收窗口要完整留给 PC。 */
  ota_proto_note_rx(&s_ota.proto, esp_timer_get_time());
  notify_ui(OTA_PHASE_RECEIVING, 0, image_size);
  reply(&result, true);
  ESP_LOGI(TAG, "ota begin: %u bytes -> %s (running %s %s)", (unsigned)image_size, target->label,
           ota_session_running_partition(), ota_session_running_version());
}

static void handle_data(const uint8_t *payload, size_t len, bool window_end)
{
  const ota_proto_result_t result =
      ota_proto_data(&s_ota.proto, payload, len, window_end, esp_timer_get_time(), ota_flash_write, NULL);
  if (result.reply) {
    reply(&result, false);
  }
  if (result.state == OTA_STATE_FAILED) {
    abort_flash_session();
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, result.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "ota data failed: code=%d received=%u/%u", (int)result.code, (unsigned)result.received,
             (unsigned)s_ota.proto.image_size);
    return;
  }
  if (s_ota.phase == OTA_PHASE_RECEIVING) {
    notify_receiving(result.received, s_ota.proto.image_size);
  }
}

static void handle_end(void)
{
  ota_proto_result_t result = ota_proto_end(&s_ota.proto, esp_timer_get_time(), ota_flash_write, NULL);
  if (result.state == OTA_STATE_FAILED || !result.finished) {
    reply(&result, false);
    abort_flash_session();
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, result.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "ota end failed: code=%d received=%u/%u", (int)result.code, (unsigned)result.received,
             (unsigned)s_ota.proto.image_size);
    return;
  }

  s_ota.phase = OTA_PHASE_VERIFYING;
  notify_ui(OTA_PHASE_VERIFYING, s_ota.proto.image_size, s_ota.proto.image_size);
  /* esp_ota_end 整体校验镜像：应用描述符、芯片标识与尾部 SHA-256。 */
  esp_err_t err = esp_ota_end(s_ota.handle);
  s_ota.handle_open = false;
  if (err != ESP_OK) {
    const ota_code_t code = err == ESP_ERR_OTA_VALIDATE_FAILED ? OTA_CODE_VERIFY_FAILED : OTA_CODE_FLASH_ERROR;
    result = ota_proto_fail(&s_ota.proto, code);
    reply(&result, false);
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, s_ota.proto.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    return;
  }

  err = esp_ota_set_boot_partition(s_ota.target);
  if (err != ESP_OK) {
    result = ota_proto_fail(&s_ota.proto, OTA_CODE_FLASH_ERROR);
    reply(&result, false);
    s_ota.phase = OTA_PHASE_FAILED;
    notify_ui(OTA_PHASE_FAILED, s_ota.proto.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return;
  }

  result = ota_proto_done(&s_ota.proto);
  reply(&result, false);
  s_ota.phase = OTA_PHASE_REBOOTING;
  notify_ui(OTA_PHASE_REBOOTING, s_ota.proto.image_size, s_ota.proto.image_size);
  ESP_LOGI(TAG, "ota done: %s is the boot partition, restarting in %d ms",
           s_ota.target != NULL ? s_ota.target->label : "?", OTA_REBOOT_DELAY_MS);
  vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
  esp_restart();
}

static void handle_slot(const ota_queue_slot_t *slot)
{
  switch (slot->type) {
  case INPUT_FRAME_TYPE_OTA_BEGIN:
    handle_begin(slot->data, slot->len);
    break;
  case INPUT_FRAME_TYPE_OTA_DATA:
    handle_data(slot->data, slot->len, slot->window_end);
    break;
  case INPUT_FRAME_TYPE_OTA_END:
    handle_end();
    break;
  default:
    break;
  }
}

/** 空闲超时：PC 端被杀或线掉了就作废会话，不让半镜像占着句柄。 */
static void check_timeout(void)
{
  const ota_proto_result_t result = ota_proto_tick(&s_ota.proto, esp_timer_get_time());
  if (!result.reply) {
    return;
  }
  reply(&result, false);
  abort_flash_session();
  s_ota.phase = OTA_PHASE_FAILED;
  notify_ui(OTA_PHASE_FAILED, result.received, s_ota.proto.image_size);
  ESP_LOGW(TAG, "ota session timed out after %d ms without data", (int)(OTA_SESSION_TIMEOUT_US / 1000LL));
}

/**
 * 回滚健康门槛：UI 首帧成功（渲染通路通）且开机满 30 秒，才把
 * 镜像标记为有效；在此之前重启，引导器回退到升级前的镜像。
 */
static void check_health(void)
{
  if (s_ota.health_confirmed || !s_ota.ui_ready) {
    return;
  }
  if (esp_timer_get_time() < OTA_HEALTH_MIN_UPTIME_US) {
    return;
  }
  s_ota.health_confirmed = true;
  if (!ota_session_pending_verify()) {
    return; /* 普通启动：无需写 otadata。 */
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "new image confirmed valid (ui first frame + %d s uptime)",
             (int)(OTA_HEALTH_MIN_UPTIME_US / 1000000LL));
    return;
  }
  s_ota.health_confirmed = false; /* 下个周期再试。 */
  ESP_LOGE(TAG, "mark app valid failed: %s", esp_err_to_name(err));
}

static void ota_task(void *param)
{
  (void)param;
  ESP_LOGI(TAG, "ota channel ready (frames 0x30-0x32 on USB-Serial/JTAG)");
  for (;;) {
    ota_queue_slot_t slot;
    if (xQueueReceive(s_ota.queue, &slot, pdMS_TO_TICKS(OTA_POLL_MS)) == pdTRUE) {
      handle_slot(&slot);
    }
    check_timeout();
    check_health();
  }
}

void ota_session_handle_frame(const input_frame_view_t *frame)
{
  if (frame == NULL || s_ota.queue == NULL) {
    return;
  }
  if (frame->payload_len > OTA_DATA_PAYLOAD_MAX) {
    ESP_LOGW(TAG, "ota frame 0x%02x too long (%u bytes), dropped", frame->type, (unsigned)frame->payload_len);
    return;
  }
  ota_queue_slot_t slot;
  slot.type = frame->type;
  slot.window_end = frame->slot == OTA_SLOT_WINDOW_END;
  slot.len = (uint16_t)frame->payload_len;
  if (slot.len > 0) {
    memcpy(slot.data, frame->payload, slot.len);
  }
  /* 队列满就丢帧：PC 端等不到窗口应答会从 ACK 的 next_seq 重发，丢掉可恢复。 */
  if (xQueueSend(s_ota.queue, &slot, 0) != pdTRUE) {
    ESP_LOGW(TAG, "ota queue full, frame 0x%02x dropped", frame->type);
  }
}

void ota_session_notify_ui_ready(void)
{
  s_ota.ui_ready = true;
}

esp_err_t ota_session_start(void)
{
  ota_proto_init(&s_ota.proto);
  s_ota.phase = OTA_PHASE_IDLE;
  /* 队列放内部 RAM：flash 写入的禁缓存窗口内不能碰 PSRAM。 */
  s_ota.queue = xQueueCreateWithCaps(OTA_QUEUE_LEN, sizeof(ota_queue_slot_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (s_ota.queue == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(ota_task, "remapad-ota", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, &s_ota.task) != pdPASS) {
    vQueueDeleteWithCaps(s_ota.queue);
    s_ota.queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

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

#include "ota_proto.h"

static const char *TAG = "remapad_ota";

/** 帧队列按发送端一个窗口 16 帧设计：接收任务只入队，绝不阻塞。 */
#define OTA_QUEUE_LEN 16u
/** 任务栈来自内部 RAM（xTaskCreate 默认），flash 写入的禁缓存窗口离不开它。 */
#define OTA_TASK_STACK 6144u
/** 与接收任务同级：两个任务按时间片轮流推进，升级期间不掉字节。 */
#define OTA_TASK_PRIO 6
/** 队列轮询周期，同时充当超时检查与健康门槛检查的心跳。 */
#define OTA_POLL_MS 200u
/** 完成后先让链路把应答帧送出去，再重启；具体等待由适配层的发送策略兜底。 */
#define OTA_REBOOT_DELAY_MS 500u
/** 健康门槛：应用就绪且开机满这么久，才确认新镜像有效。 */
#define OTA_HEALTH_MIN_UPTIME_US (30 * 1000 * 1000LL)

/** 队列槽：载荷最大的是 DATA 帧（序号 + 200 字节数据）。 */
typedef struct {
  ota_session_msg_t msg;
  /** 该数据帧带窗口末帧标记，收到即回应答。 */
  bool window_end;
  uint16_t len;
  uint8_t data[OTA_DATA_PAYLOAD_MAX];
} ota_queue_slot_t;

static struct {
  QueueHandle_t queue;
  TaskHandle_t task;
  ota_session_port_t port;
  ota_proto_t proto;
  esp_ota_handle_t handle;
  const esp_partition_t *target;
  bool handle_open;
  volatile bool app_ready;
  bool health_confirmed;
  ota_session_phase_t phase;
  /** 上次广播的整数百分比：数据帧按 1% 粒度限频，不逐帧打扰 port。 */
  uint32_t reported_pct;
} s_ota;

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

/** 经 port 广播阶段/进度：port 未注入进度出口时静默跳过。 */
static void emit_progress(ota_session_phase_t phase, uint32_t received, uint32_t total)
{
  if (s_ota.port.on_progress == NULL) {
    return;
  }
  s_ota.port.on_progress(phase, received, total, s_ota.port.user);
}

/** 接收进度按整数百分比限频广播：同一百分比的数据帧不重复进 port。 */
static void emit_receiving(uint32_t received, uint32_t total)
{
  const uint32_t pct = total > 0u ? (uint32_t)((uint64_t)received * 100u / total) : 0u;
  if (pct == s_ota.reported_pct) {
    return;
  }
  s_ota.reported_pct = pct;
  emit_progress(OTA_SESSION_PHASE_RECEIVING, received, total);
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

/** 回一帧 ACK；BEGIN 的应答带 16 字节运行版本，便于发送端显示升级方向。 */
static void reply(const ota_proto_result_t *result, bool with_version)
{
  uint8_t payload[OTA_ACK_PAYLOAD_LEN + OTA_ACK_VERSION_LEN];
  const size_t len =
      ota_proto_encode_ack(result, ota_session_running_version(), with_version, payload, sizeof(payload));
  if (len == 0) {
    return;
  }
  if (s_ota.port.send_ack != NULL) {
    s_ota.port.send_ack(payload, len, s_ota.port.user);
  }
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
    /* 首字节 magic 不对：送来的不是合法的 ESP32 应用镜像（这里只验结构，不认项目身份）。 */
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
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, 0, 0);
    ESP_LOGW(TAG, "ota begin rejected: bad header");
    return;
  }
  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  if (target == NULL) {
    const ota_proto_result_t result = ota_proto_fail(&s_ota.proto, OTA_CODE_BUSY);
    reply(&result, true);
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, 0, 0);
    ESP_LOGE(TAG, "no update partition available");
    return;
  }
  /* 会话占用中（接收中或刚收完等校验）或尺寸越界都在这里被挡下，不碰 flash。 */
  ota_proto_result_t result = ota_proto_begin(&s_ota.proto, image_size, target->size, now);
  if (result.state != OTA_STATE_RECEIVING) {
    reply(&result, true);
    if (result.code != OTA_CODE_BUSY) {
      s_ota.phase = OTA_SESSION_PHASE_FAILED;
      emit_progress(OTA_SESSION_PHASE_FAILED, 0, image_size);
    }
    ESP_LOGW(TAG, "ota begin refused (code=%d, image=%u, partition=%s %u bytes)", (int)result.code,
             (unsigned)image_size, target->label, (unsigned)target->size);
    return;
  }
  if (s_ota.handle_open) {
    /* 接收中的重复 BEGIN（应答丢失后的重发）：协议层已按幂等放行，这里只补发
     * 应答——flash 句柄还开着，重入 esp_ota_begin 会立刻 ALREADY_IN_PROGRESS。 */
    reply(&result, true);
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
      s_ota.phase = OTA_SESSION_PHASE_FAILED;
      emit_progress(OTA_SESSION_PHASE_FAILED, 0, image_size);
    }
    ESP_LOGE(TAG, "esp_ota_begin failed: %s (image=%u)", esp_err_to_name(err), (unsigned)image_size);
    return;
  }
  s_ota.handle = handle;
  s_ota.handle_open = true;
  s_ota.target = target;
  s_ota.phase = OTA_SESSION_PHASE_RECEIVING;
  s_ota.reported_pct = 0;
  /* 空闲超时从应答时刻起算：预擦已经过去，接收窗口要完整留给发送端。 */
  ota_proto_note_rx(&s_ota.proto, esp_timer_get_time());
  emit_progress(OTA_SESSION_PHASE_RECEIVING, 0, image_size);
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
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, result.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "ota data failed: code=%d received=%u/%u", (int)result.code, (unsigned)result.received,
             (unsigned)s_ota.proto.image_size);
    return;
  }
  if (s_ota.phase == OTA_SESSION_PHASE_RECEIVING) {
    emit_receiving(result.received, s_ota.proto.image_size);
  }
}

static void handle_end(void)
{
  ota_proto_result_t result = ota_proto_end(&s_ota.proto, esp_timer_get_time(), ota_flash_write, NULL);
  if (!result.finished && result.state == OTA_STATE_DONE) {
    /* 重复 END（done 应答丢失后的重发）：只补发应答，校验与重启不重入。 */
    reply(&result, false);
    return;
  }
  if (result.state == OTA_STATE_FAILED || !result.finished) {
    reply(&result, false);
    abort_flash_session();
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, result.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "ota end failed: code=%d received=%u/%u", (int)result.code, (unsigned)result.received,
             (unsigned)s_ota.proto.image_size);
    return;
  }

  s_ota.phase = OTA_SESSION_PHASE_VERIFYING;
  emit_progress(OTA_SESSION_PHASE_VERIFYING, s_ota.proto.image_size, s_ota.proto.image_size);
  /* esp_ota_end 整体校验镜像：应用描述符、芯片标识与尾部 SHA-256。 */
  esp_err_t err = esp_ota_end(s_ota.handle);
  s_ota.handle_open = false;
  if (err != ESP_OK) {
    const ota_code_t code = err == ESP_ERR_OTA_VALIDATE_FAILED ? OTA_CODE_VERIFY_FAILED : OTA_CODE_FLASH_ERROR;
    result = ota_proto_fail(&s_ota.proto, code);
    reply(&result, false);
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, s_ota.proto.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    return;
  }

  err = esp_ota_set_boot_partition(s_ota.target);
  if (err != ESP_OK) {
    result = ota_proto_fail(&s_ota.proto, OTA_CODE_FLASH_ERROR);
    reply(&result, false);
    s_ota.phase = OTA_SESSION_PHASE_FAILED;
    emit_progress(OTA_SESSION_PHASE_FAILED, s_ota.proto.received, s_ota.proto.image_size);
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return;
  }

  result = ota_proto_done(&s_ota.proto);
  reply(&result, false);
  s_ota.phase = OTA_SESSION_PHASE_REBOOTING;
  emit_progress(OTA_SESSION_PHASE_REBOOTING, s_ota.proto.image_size, s_ota.proto.image_size);
  ESP_LOGI(TAG, "ota done: %s is the boot partition, restarting in %d ms",
           s_ota.target != NULL ? s_ota.target->label : "?", OTA_REBOOT_DELAY_MS);
  vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
  esp_restart();
}

static void handle_slot(const ota_queue_slot_t *slot)
{
  switch (slot->msg) {
  case OTA_SESSION_MSG_BEGIN:
    handle_begin(slot->data, slot->len);
    break;
  case OTA_SESSION_MSG_DATA:
    handle_data(slot->data, slot->len, slot->window_end);
    break;
  case OTA_SESSION_MSG_END:
    handle_end();
    break;
  default:
    break;
  }
}

/** 空闲超时：发送端被杀或线掉了就作废会话，不让半镜像占着句柄。 */
static void check_timeout(void)
{
  const ota_proto_result_t result = ota_proto_tick(&s_ota.proto, esp_timer_get_time());
  if (!result.reply) {
    return;
  }
  reply(&result, false);
  abort_flash_session();
  s_ota.phase = OTA_SESSION_PHASE_FAILED;
  emit_progress(OTA_SESSION_PHASE_FAILED, result.received, s_ota.proto.image_size);
  ESP_LOGW(TAG, "ota session timed out after %d ms without data", (int)(OTA_SESSION_TIMEOUT_US / 1000LL));
}

/**
 * 回滚健康门槛：应用就绪（装配方定义，本工程是核心服务启动完成）且开机满 30 秒，
 * 才把镜像标记为有效；在此之前重启，引导器回退到升级前的镜像。
 */
static void check_health(void)
{
  if (s_ota.health_confirmed || !s_ota.app_ready) {
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
    ESP_LOGI(TAG, "new image confirmed valid (app ready + %d s uptime)", (int)(OTA_HEALTH_MIN_UPTIME_US / 1000000LL));
    return;
  }
  s_ota.health_confirmed = false; /* 下个周期再试。 */
  ESP_LOGE(TAG, "mark app valid failed: %s", esp_err_to_name(err));
}

static void ota_task(void *param)
{
  (void)param;
  ESP_LOGI(TAG, "ota session core ready");
  for (;;) {
    ota_queue_slot_t slot;
    if (xQueueReceive(s_ota.queue, &slot, pdMS_TO_TICKS(OTA_POLL_MS)) == pdTRUE) {
      handle_slot(&slot);
    }
    check_timeout();
    check_health();
  }
}

void ota_session_handle_frame(ota_session_msg_t msg, bool window_end, const uint8_t *payload, size_t len)
{
  if (s_ota.queue == NULL || msg > OTA_SESSION_MSG_END) {
    return;
  }
  if (len > OTA_DATA_PAYLOAD_MAX) {
    ESP_LOGW(TAG, "ota message %d too long (%u bytes), dropped", (int)msg, (unsigned)len);
    return;
  }
  ota_queue_slot_t slot;
  slot.msg = msg;
  slot.window_end = window_end;
  slot.len = (uint16_t)len;
  if (slot.len > 0) {
    memcpy(slot.data, payload, slot.len);
  }
  /* 队列满就丢帧：发送端等不到窗口应答会从 ACK 的 next_seq 重发，丢掉可恢复。 */
  if (xQueueSend(s_ota.queue, &slot, 0) != pdTRUE) {
    ESP_LOGW(TAG, "ota queue full, message %d dropped", (int)msg);
  }
}

void ota_session_notify_ready(void)
{
  s_ota.app_ready = true;
}

esp_err_t ota_session_start(const ota_session_port_t *port)
{
  if (port == NULL || port->send_ack == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  ota_proto_init(&s_ota.proto);
  s_ota.port = *port;
  s_ota.phase = OTA_SESSION_PHASE_IDLE;
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

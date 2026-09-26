/**
 * 屏幕 UI 提供者（Slint 集成层）：owner task 上初始化面板/触摸/背光与启动画面，
 * 把界面事件循环跑起来；状态装配与动作分发都在固件核心的 ui_service 里，
 * 本文件只做硬件装配与界面通路（截图回传、渲染统计），不碰业务状态。
 */
#include "ui_service.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_config.h"
#include "backlight.h"
#include "boot_splash.h"
#include "input_frame.h"
#include "input_link.h"
#include "panel.h"
#include "slint_ui.h"
#include "touch.h"

static const char *TAG = "remapad_ui";

/** owner task 栈：界面事件循环 + 渲染都在它上面，栈放内部 RAM 以保证渲染速度。 */
#define REMAPAD_UI_TASK_STACK_BYTES (64U * 1024U)
#define REMAPAD_UI_TASK_NAME "remapad-ui"
#define REMAPAD_UI_TASK_PRIORITY 5
/** 与数据面任务同核：BLE 控制器与 NimBLE 主机栈都在 CPU0。 */
#define REMAPAD_UI_TASK_CORE 1
/** 持久化亮度缺失时的兜底值。 */
#define REMAPAD_BACKLIGHT_PCT_DEFAULT 40
/** 启动阶段权重（毫秒）：建窗与首帧。 */
#define REMAPAD_UI_STAGE_MS_COUNT 4
static const uint32_t REMAPAD_UI_STAGE_MS[REMAPAD_UI_STAGE_MS_COUNT] = {
  60, /* panel + touch + platform */
  60, /* App::create + 首轮状态 */
  80, /* 首帧渲染与提交 */
  20, /* 进入事件循环 */
};
/** 截图分块回传的等待上限。 */
#define REMAPAD_SHOT_TX_TIMEOUT_MS 200U

static atomic_bool s_shot_requested;

/** 截图的应答在状态轮询里同步回传（下面的实现体在其后）。 */
static void remapad_ui_stream_shot(void);

/** 面板提交入口：界面平台把行带交给它，驱动负责字节序与 DMA 等待。 */
static esp_err_t ui_panel_transfer(uint16_t *pixels, int x, int y, int width, int height)
{
  return panel_transfer(pixels, x, y, width, height);
}

/** 触摸采样入口：息屏期间由平台整段跳过，这里只做一次采样。 */
static size_t ui_touch_sample(remapad_slint_touch_t *out, size_t capacity)
{
  touch_contact_t contacts[4];
  size_t count = touch_sample(contacts, capacity < 4U ? capacity : 4U);
  for (size_t index = 0; index < count; ++index) {
    out[index].x = contacts[index].x;
    out[index].y = contacts[index].y;
  }
  return count;
}

/** 状态轮询回调（UI 任务上下文）：装配在 core，截图请求在本任务上结算。 */
static void ui_poll(remapad_ui_state_t *state, void *user)
{
  (void)user;
  ui_service_fill_state(state);
  if (atomic_exchange_explicit(&s_shot_requested, false, memory_order_relaxed)) {
    remapad_ui_stream_shot();
  }
}

/** 动作回调（UI 任务上下文）：全部交回 core 的统一分发。 */
static void ui_action(const char *name, int value, void *user)
{
  (void)user;
  ui_service_handle_action(name, value);
}

/** 整屏回传：截图通路把当前帧缓冲按分块上限交给 PC。 */
static void remapad_ui_stream_shot(void)
{
  if (!input_link_active()) {
    return;
  }
  const uint16_t *frame = remapad_slint_ui_frame();
  if (frame == NULL) {
    return;
  }
  const uint16_t width = REMAPAD_SLINT_VIEW_WIDTH;
  const uint16_t height = REMAPAD_SLINT_VIEW_HEIGHT;
  if (input_link_send_image_info(width, height, REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
    ESP_LOGW(TAG, "shot info frame dropped");
    return;
  }
  const uint8_t *bytes = (const uint8_t *)frame;
  const uint32_t total = (uint32_t)width * height * sizeof(uint16_t);
  uint32_t offset = 0;
  while (offset < total) {
    size_t piece = total - offset;
    if (piece > INPUT_FRAME_IMAGE_CHUNK_MAX) {
      piece = INPUT_FRAME_IMAGE_CHUNK_MAX;
    }
    if (input_link_send_image_data(offset, bytes + offset, piece, REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
      ESP_LOGW(TAG, "shot aborted at %u/%u bytes", (unsigned)offset, (unsigned)total);
      return;
    }
    offset += (uint32_t)piece;
  }
  if (input_link_send_image_end(total, REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
    ESP_LOGW(TAG, "shot end frame dropped");
    return;
  }
  ESP_LOGI(TAG, "shot sent: %ux%u (%u bytes)", (unsigned)width, (unsigned)height, (unsigned)total);
}

void remapad_ui_request_shot(void)
{
  atomic_store_explicit(&s_shot_requested, true, memory_order_relaxed);
}

void remapad_ui_request_trace(unsigned frames)
{
  if (frames == 0U) {
    frames = REMAPAD_UI_TRACE_FRAMES_DEFAULT;
  }
  remapad_slint_ui_trace_frames(frames);
}

static void ui_owner_task(void *opaque)
{
  (void)opaque;
  /* 面板与触摸初始化失败不阻断启动：面板失败时界面仍渲染进 PSRAM。 */
  if (panel_init() != ESP_OK) {
    ESP_LOGE(TAG, "panel init failed");
  }
  if (touch_init() != ESP_OK) {
    ESP_LOGE(TAG, "touch init failed");
  }
  if (backlight_init() != ESP_OK) {
    ESP_LOGE(TAG, "backlight init failed");
  }
  const uint8_t brightness =
      app_config_get()->brightness != 0U ? app_config_get()->brightness : REMAPAD_BACKLIGHT_PCT_DEFAULT;
  /* 面板就绪后立刻画启动画面并点亮背光：界面建窗与首帧期间屏幕已有内容。 */
  if (boot_splash_begin(brightness, REMAPAD_UI_STAGE_MS, REMAPAD_UI_STAGE_MS_COUNT) != ESP_OK) {
    ESP_LOGW(TAG, "boot splash unavailable");
  }

  const remapad_slint_hooks_t hooks = {
    .transfer = ui_panel_transfer,
    .touch_sample = ui_touch_sample,
  };
  if (remapad_slint_ui_start(&hooks, ui_poll, ui_action, NULL) != ESP_OK) {
    boot_splash_fail();
    ESP_LOGE(TAG, "ui start failed (internal=%u largest=%u psram=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    vTaskDelete(NULL);
    return;
  }
  /* 首帧之前交出启动画面：界面的第一帧是全屏重画，动画任务再画会打架。 */
  boot_splash_end();
  ESP_LOGI(TAG, "ui ready on CPU%d", (int)xPortGetCoreID());
  remapad_slint_ui_loop();
  vTaskDelete(NULL);
}

esp_err_t remapad_ui_start(void)
{
  atomic_init(&s_shot_requested, false);
  ui_service_reset();
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      ui_owner_task, REMAPAD_UI_TASK_NAME, REMAPAD_UI_TASK_STACK_BYTES, NULL, REMAPAD_UI_TASK_PRIORITY, NULL,
      REMAPAD_UI_TASK_CORE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "owner task create failed");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

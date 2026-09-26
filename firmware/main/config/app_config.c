#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "remapad_config";

#define CONFIG_NS "remapad"
#define CONFIG_KEY "cfg"

/** 序列化格式：版本字节 + 字段（尾部保留对齐）。v1 长度 16；引入固件版本
 * 字段后扩到 24（[16..18] 固件版本），读回兼容 16 字节旧记录（新字段用
 * 默认值），首次保存即写新长度。[14] 是 DS 手柄行为：bit7 是有效标记，
 * bit0 触摸板映射加减键、bit1 截图键关闭——截图键默认是开，零值不能直接
 * 当默认，因此这一字节带标记位（旧记录该字节为 0，两项都用默认值）。 */
#define CONFIG_BLOB_LEN 24
#define CONFIG_BLOB_VERSION 1
/** 旧版（无固件版本字段）的记录长度。 */
#define CONFIG_BLOB_LEN_V1 16

#define CONFIG_DEFAULT_BRIGHTNESS 40

/** 落盘检查周期：设置项改动只置内存表的脏标记，由提交任务每 1 分钟检查
 * 一次，确有改动才写一次 NVS。每次落盘都要擦写 flash 页，切选项这类高频
 * 改动不能改一次写一次；代价是断电会丢掉最近一个周期内的改动。 */
#define CONFIG_COMMIT_PERIOD_MS (60 * 1000)

static struct {
  app_config_t cfg;
  SemaphoreHandle_t lock;
  /** 提交任务的就绪信号：周期超时或被 app_config_flush() 提前叫醒。 */
  SemaphoreHandle_t wake;
  /** 内存表存在未落盘的改动（由 lock 保护）。 */
  bool dirty;
} s_appcfg;

/** 锁内序列化配置快照。 */
static void serialize_locked(uint8_t blob[CONFIG_BLOB_LEN])
{
  memset(blob, 0, CONFIG_BLOB_LEN);
  blob[0] = CONFIG_BLOB_VERSION;
  blob[1] = s_appcfg.cfg.brightness;
  blob[2] = s_appcfg.cfg.screen_on ? 1u : 0u;
  /* [3] 曾是 USB 角色：该字段不落盘（重启恒为串口），保留字节写 0。 */
  blob[3] = 0;
  /* [4] 曾是手柄形态（Pro / JoyCon）：设备只模拟 Pro Controller 2，保留字节写 0。 */
  blob[4] = 0;
  blob[5] = (uint8_t)(s_appcfg.cfg.body_color >> 16);
  blob[6] = (uint8_t)(s_appcfg.cfg.body_color >> 8);
  blob[7] = (uint8_t)(s_appcfg.cfg.body_color);
  blob[8] = (uint8_t)(s_appcfg.cfg.button_color >> 16);
  blob[9] = (uint8_t)(s_appcfg.cfg.button_color >> 8);
  blob[10] = (uint8_t)(s_appcfg.cfg.button_color);
  blob[11] = (uint8_t)(s_appcfg.cfg.grip_color >> 16);
  blob[12] = (uint8_t)(s_appcfg.cfg.grip_color >> 8);
  blob[13] = (uint8_t)(s_appcfg.cfg.grip_color);
  blob[16] = s_appcfg.cfg.fw_version[0];
  blob[17] = s_appcfg.cfg.fw_version[1];
  blob[18] = s_appcfg.cfg.fw_version[2];
  blob[14] = (uint8_t)(0x80u | (s_appcfg.cfg.ds_touchpad_plus_minus ? 0x01u : 0u) |
                       (s_appcfg.cfg.ds_capture_key ? 0u : 0x02u));
  /* [19..21] 高光配色：后加字段，旧记录（同长度、该段为零）读出即「未配置」。 */
  blob[19] = (uint8_t)(s_appcfg.cfg.accent_color >> 16);
  blob[20] = (uint8_t)(s_appcfg.cfg.accent_color >> 8);
  blob[21] = (uint8_t)(s_appcfg.cfg.accent_color);
}

/** 置脏标记：改动只落在内存表，落盘由提交任务的周期检查统一完成。 */
static void mark_dirty(void)
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.dirty = true;
    xSemaphoreGive(s_appcfg.lock);
  }
}

/** 写一次 NVS；失败保留脏标记，下个周期重试。 */
static esp_err_t write_blob(const uint8_t *blob)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(CONFIG_NS, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
    return err;
  }
  err = nvs_set_blob(handle, CONFIG_KEY, blob, CONFIG_BLOB_LEN);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
  }
  return err;
}

/** NVS 提交任务：内部 RAM 栈（ble_creds 同款约束，见模块头注释）。
 * 每 CONFIG_COMMIT_PERIOD_MS 醒一次，脏标记为假时直接回去睡，不碰 flash。 */
static void commit_task(void *param)
{
  (void)param;
  for (;;) {
    xSemaphoreTake(s_appcfg.wake, pdMS_TO_TICKS(CONFIG_COMMIT_PERIOD_MS));
    if (s_appcfg.lock == NULL || xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (!s_appcfg.dirty) {
      xSemaphoreGive(s_appcfg.lock);
      continue;
    }
    uint8_t blob[CONFIG_BLOB_LEN];
    serialize_locked(blob);
    s_appcfg.dirty = false;
    xSemaphoreGive(s_appcfg.lock);
    if (write_blob(blob) != ESP_OK) {
      mark_dirty();
    }
  }
}

esp_err_t app_config_init(void)
{
  s_appcfg.lock = xSemaphoreCreateMutex();
  s_appcfg.wake = xSemaphoreCreateBinary();
  if (s_appcfg.lock == NULL || s_appcfg.wake == NULL ||
      xTaskCreate(commit_task, "appcfg", 4096, NULL, 2, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  s_appcfg.cfg.brightness = CONFIG_DEFAULT_BRIGHTNESS;
  s_appcfg.cfg.screen_on = true;
  s_appcfg.cfg.usb_role = APP_CONFIG_USB_DEVICE;
  s_appcfg.cfg.body_color = 0;
  s_appcfg.cfg.button_color = 0;
  s_appcfg.cfg.accent_color = 0;
  s_appcfg.cfg.grip_color = 0;
  /* 上报固件版本：出厂值固化在 app_config.h 的 CONFIG_DEFAULT_FW_VERSION_*。 */
  s_appcfg.cfg.fw_version[0] = CONFIG_DEFAULT_FW_VERSION_MAJOR;
  s_appcfg.cfg.fw_version[1] = CONFIG_DEFAULT_FW_VERSION_MINOR;
  s_appcfg.cfg.fw_version[2] = CONFIG_DEFAULT_FW_VERSION_REVISION;
  /* DS 手柄行为：触摸板映射加减键默认关、截图键默认开。 */
  s_appcfg.cfg.ds_touchpad_plus_minus = false;
  s_appcfg.cfg.ds_capture_key = true;

  nvs_handle_t handle;
  const esp_err_t err = nvs_open(CONFIG_NS, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (err != ESP_OK) {
    return err;
  }
  uint8_t blob[CONFIG_BLOB_LEN];
  size_t len = sizeof(blob);
  const esp_err_t get = nvs_get_blob(handle, CONFIG_KEY, blob, &len);
  nvs_close(handle);
  if (get == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  if (get != ESP_OK) {
    return get;
  }
  if ((len != CONFIG_BLOB_LEN && len != CONFIG_BLOB_LEN_V1) || blob[0] != CONFIG_BLOB_VERSION) {
    ESP_LOGW(TAG, "corrupted config blob (len=%u v=%u), using defaults", (unsigned)len, (unsigned)blob[0]);
    return ESP_OK;
  }
  s_appcfg.cfg.brightness = blob[1] > 100 ? 100u : blob[1];
  /* 息屏状态不跨重启保留：复位后恒为亮屏，NVS 值仅作落盘格式占位。 */
  s_appcfg.cfg.screen_on = true;
  /* USB 角色不跨重启保留：开机恒为串口（device），旧记录里的角色一并忽略。 */
  s_appcfg.cfg.usb_role = APP_CONFIG_USB_DEVICE;
  s_appcfg.cfg.body_color = ((uint32_t)blob[5] << 16) | ((uint32_t)blob[6] << 8) | blob[7];
  s_appcfg.cfg.button_color = ((uint32_t)blob[8] << 16) | ((uint32_t)blob[9] << 8) | blob[10];
  s_appcfg.cfg.grip_color = ((uint32_t)blob[11] << 16) | ((uint32_t)blob[12] << 8) | blob[13];
  if (len >= CONFIG_BLOB_LEN) {
    s_appcfg.cfg.fw_version[0] = blob[16];
    s_appcfg.cfg.fw_version[1] = blob[17];
    s_appcfg.cfg.fw_version[2] = blob[18];
    s_appcfg.cfg.accent_color = ((uint32_t)blob[19] << 16) | ((uint32_t)blob[20] << 8) | blob[21];
    if ((blob[14] & 0x80u) != 0) {
      s_appcfg.cfg.ds_touchpad_plus_minus = (blob[14] & 0x01u) != 0;
      s_appcfg.cfg.ds_capture_key = (blob[14] & 0x02u) == 0;
    }
  }
  ESP_LOGI(TAG, "loaded config: brightness=%u screen=%u role=%u", s_appcfg.cfg.brightness,
           (unsigned)s_appcfg.cfg.screen_on, (unsigned)s_appcfg.cfg.usb_role);
  return ESP_OK;
}

const app_config_t *app_config_get(void)
{
  return &s_appcfg.cfg;
}

void app_config_set_brightness(uint8_t pct)
{
  if (pct > 100) {
    pct = 100;
  }
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.brightness = pct;
    xSemaphoreGive(s_appcfg.lock);
  }
  mark_dirty();
}

void app_config_set_screen_on(bool on)
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.screen_on = on;
    xSemaphoreGive(s_appcfg.lock);
  }
  mark_dirty();
}

void app_config_set_usb_role(app_config_usb_role_t role)
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.usb_role = role == APP_CONFIG_USB_HOST ? APP_CONFIG_USB_HOST : APP_CONFIG_USB_DEVICE;
    xSemaphoreGive(s_appcfg.lock);
  }
}

void app_config_set_controller_colors(uint32_t body_rgb, uint32_t button_rgb, uint32_t accent_rgb, uint32_t grip_rgb)
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.body_color = body_rgb;
    s_appcfg.cfg.button_color = button_rgb;
    s_appcfg.cfg.accent_color = accent_rgb;
    s_appcfg.cfg.grip_color = grip_rgb;
    xSemaphoreGive(s_appcfg.lock);
  }
  mark_dirty();
}

void app_config_set_fw_version(const uint8_t ver[3])
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.fw_version[0] = ver[0];
    s_appcfg.cfg.fw_version[1] = ver[1];
    s_appcfg.cfg.fw_version[2] = ver[2];
    xSemaphoreGive(s_appcfg.lock);
  }
  mark_dirty();
}

void app_config_set_ds_behavior(bool touchpad_plus_minus, bool capture_key)
{
  if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
    s_appcfg.cfg.ds_touchpad_plus_minus = touchpad_plus_minus;
    s_appcfg.cfg.ds_capture_key = capture_key;
    xSemaphoreGive(s_appcfg.lock);
  }
  mark_dirty();
}

void app_config_flush(void)
{
  if (s_appcfg.wake != NULL) {
    xSemaphoreGive(s_appcfg.wake);
  }
}

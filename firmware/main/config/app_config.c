#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "remapad_config";

#define CONFIG_NS "remapad"
#define CONFIG_KEY "cfg"

/** 序列化格式：版本字节 + 字段（尾部保留对齐）。v1 长度 16；引入固件版本
 * 字段后扩到 24（[16..18] 固件版本），读回兼容 16 字节旧记录（新字段用
 * 默认值），首次保存即写新长度。 */
#define CONFIG_BLOB_LEN 24
#define CONFIG_BLOB_VERSION 1
/** 旧版（无固件版本字段）的记录长度。 */
#define CONFIG_BLOB_LEN_V1 16

#define CONFIG_COMMIT_QUEUE_LEN 4

#define CONFIG_DEFAULT_BRIGHTNESS 40

static struct {
    app_config_t cfg;
    SemaphoreHandle_t lock;
    QueueHandle_t commit_queue;
} s_appcfg;

/** NVS 写任务：内部 RAM 栈（ble_creds 同款约束，见模块头注释）。 */
static void commit_task(void *param)
{
    uint8_t blob[CONFIG_BLOB_LEN];
    while (xQueueReceive(s_appcfg.commit_queue, blob, portMAX_DELAY) == pdTRUE) {
        nvs_handle_t handle;
        const esp_err_t err = nvs_open(CONFIG_NS, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
            continue;
        }
        esp_err_t set = nvs_set_blob(handle, CONFIG_KEY, blob, CONFIG_BLOB_LEN);
        if (set == ESP_OK) {
            set = nvs_commit(handle);
        }
        nvs_close(handle);
        if (set != ESP_OK) {
            ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(set));
        }
    }
}

/** 锁内序列化配置快照。 */
static void serialize_locked(uint8_t blob[CONFIG_BLOB_LEN])
{
    memset(blob, 0, CONFIG_BLOB_LEN);
    blob[0] = CONFIG_BLOB_VERSION;
    blob[1] = s_appcfg.cfg.brightness;
    blob[2] = s_appcfg.cfg.screen_on ? 1u : 0u;
    blob[3] = s_appcfg.cfg.usb_role;
    blob[4] = s_appcfg.cfg.ctrl_type;
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
}

static void schedule_commit(void)
{
    if (s_appcfg.commit_queue == NULL) {
        return;
    }
    uint8_t blob[CONFIG_BLOB_LEN];
    if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
        serialize_locked(blob);
        xSemaphoreGive(s_appcfg.lock);
    }
    xQueueSend(s_appcfg.commit_queue, blob, portMAX_DELAY);
}

esp_err_t app_config_init(void)
{
    s_appcfg.lock = xSemaphoreCreateMutex();
    s_appcfg.commit_queue = xQueueCreate(CONFIG_COMMIT_QUEUE_LEN, CONFIG_BLOB_LEN);
    if (s_appcfg.lock == NULL || s_appcfg.commit_queue == NULL ||
        xTaskCreate(commit_task, "appcfg", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_appcfg.cfg.brightness = CONFIG_DEFAULT_BRIGHTNESS;
    s_appcfg.cfg.screen_on = true;
    s_appcfg.cfg.usb_role = APP_CONFIG_USB_DEVICE;
    s_appcfg.cfg.ctrl_type = APP_CONFIG_CTRL_PRO;
    s_appcfg.cfg.body_color = 0;
    s_appcfg.cfg.button_color = 0;
    s_appcfg.cfg.grip_color = 0;
    /* 上报固件版本默认 1.6.1（与出厂块历史值一致，高于抓包样本 1.0.14）。 */
    s_appcfg.cfg.fw_version[0] = 0x01;
    s_appcfg.cfg.fw_version[1] = 0x06;
    s_appcfg.cfg.fw_version[2] = 0x01;

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
        ESP_LOGW(TAG, "corrupted config blob (len=%u v=%u), using defaults",
                 (unsigned)len, (unsigned)blob[0]);
        return ESP_OK;
    }
    s_appcfg.cfg.brightness = blob[1] > 100 ? 100u : blob[1];
    /* 息屏状态不跨重启保留：复位后恒为亮屏，NVS 值仅作落盘格式占位。 */
    s_appcfg.cfg.screen_on = true;
    s_appcfg.cfg.usb_role = blob[3] == APP_CONFIG_USB_HOST ? APP_CONFIG_USB_HOST
                                                           : APP_CONFIG_USB_DEVICE;
    s_appcfg.cfg.ctrl_type = blob[4] == APP_CONFIG_CTRL_JOYCON ? APP_CONFIG_CTRL_JOYCON
                                                               : APP_CONFIG_CTRL_PRO;
    s_appcfg.cfg.body_color = ((uint32_t)blob[5] << 16) | ((uint32_t)blob[6] << 8) | blob[7];
    s_appcfg.cfg.button_color = ((uint32_t)blob[8] << 16) | ((uint32_t)blob[9] << 8) | blob[10];
    s_appcfg.cfg.grip_color = ((uint32_t)blob[11] << 16) | ((uint32_t)blob[12] << 8) | blob[13];
    if (len >= CONFIG_BLOB_LEN) {
        s_appcfg.cfg.fw_version[0] = blob[16];
        s_appcfg.cfg.fw_version[1] = blob[17];
        s_appcfg.cfg.fw_version[2] = blob[18];
    }
    ESP_LOGI(TAG, "loaded config: brightness=%u screen=%u role=%u type=%u",
             s_appcfg.cfg.brightness, (unsigned)s_appcfg.cfg.screen_on,
             (unsigned)s_appcfg.cfg.usb_role, (unsigned)s_appcfg.cfg.ctrl_type);
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
    schedule_commit();
}

void app_config_set_screen_on(bool on)
{
    if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
        s_appcfg.cfg.screen_on = on;
        xSemaphoreGive(s_appcfg.lock);
    }
    schedule_commit();
}

void app_config_set_usb_role(app_config_usb_role_t role)
{
    if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
        s_appcfg.cfg.usb_role = role == APP_CONFIG_USB_HOST ? APP_CONFIG_USB_HOST
                                                            : APP_CONFIG_USB_DEVICE;
        xSemaphoreGive(s_appcfg.lock);
    }
    schedule_commit();
}

void app_config_set_controller(app_config_ctrl_type_t type,
                               uint32_t body_rgb, uint32_t button_rgb, uint32_t grip_rgb)
{
    if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
        s_appcfg.cfg.ctrl_type = type == APP_CONFIG_CTRL_JOYCON ? APP_CONFIG_CTRL_JOYCON
                                                                : APP_CONFIG_CTRL_PRO;
        s_appcfg.cfg.body_color = body_rgb;
        s_appcfg.cfg.button_color = button_rgb;
        s_appcfg.cfg.grip_color = grip_rgb;
        xSemaphoreGive(s_appcfg.lock);
    }
    schedule_commit();
}

void app_config_set_fw_version(const uint8_t ver[3])
{
    if (s_appcfg.lock != NULL && xSemaphoreTake(s_appcfg.lock, portMAX_DELAY) == pdTRUE) {
        s_appcfg.cfg.fw_version[0] = ver[0];
        s_appcfg.cfg.fw_version[1] = ver[1];
        s_appcfg.cfg.fw_version[2] = ver[2];
        xSemaphoreGive(s_appcfg.lock);
    }
    schedule_commit();
}

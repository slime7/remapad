#include "ble_creds.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "remapad_blcred";

#define CREDS_NS "remapad"
#define CREDS_KEY "pairing"

/** 序列化格式：1B 条数 + 最多 NS2_CREDS_MAX 条记录（6B MAC + 16B LTK）。 */
#define CREDS_BLOB_LEN (1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN))
#define CREDS_COMMIT_QUEUE_LEN 4

static struct {
    ns2_cred_record_t records[NS2_CREDS_MAX];
    size_t count;
} s_creds;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commit_queue;

/** NVS 写任务：PocketJS owner task 的栈在 PSRAM，而 flash 写入期间缓存被
 * 禁用，此时访问 PSRAM（含任务自身的栈）会触发 cache 异常重启——「停止
 * 配对即重启」的根因。所有凭证落盘因此收敛到这个内部 RAM 栈的任务执行，
 * 调用方只更新内存表并投递快照，任何任务上下文都不会再写 flash。 */
static void commit_task(void *param)
{
    uint8_t blob[CREDS_BLOB_LEN];
    while (xQueueReceive(s_commit_queue, blob, portMAX_DELAY) == pdTRUE) {
        nvs_handle_t handle;
        const esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
            continue;
        }
        esp_err_t set = nvs_set_blob(handle, CREDS_KEY, blob, CREDS_BLOB_LEN);
        if (set == ESP_OK) {
            set = nvs_commit(handle);
        }
        nvs_close(handle);
        if (set != ESP_OK) {
            ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(set));
        } else {
            ESP_LOGI(TAG, "committed %u pairing record(s)", blob[0]);
        }
    }
}

/** 锁内序列化当前凭证表为固定长度快照（尾部补零，读回按 blob[0] 条数解析）。 */
static void serialize_locked(uint8_t blob[CREDS_BLOB_LEN])
{
    memset(blob, 0, CREDS_BLOB_LEN);
    blob[0] = (uint8_t)s_creds.count;
    for (size_t i = 0; i < s_creds.count; i++) {
        uint8_t *rec = &blob[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(rec, s_creds.records[i].mac, NS2_CREDS_MAC_LEN);
        memcpy(rec + NS2_CREDS_MAC_LEN, s_creds.records[i].ltk, NS2_CREDS_LTK_LEN);
    }
}

esp_err_t ble_creds_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_commit_queue = xQueueCreate(CREDS_COMMIT_QUEUE_LEN, CREDS_BLOB_LEN);
    if (s_lock == NULL || s_commit_queue == NULL ||
        xTaskCreate(commit_task, "blecreds", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_creds.count = 0;
    nvs_handle_t handle;
    const esp_err_t err = nvs_open(CREDS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t blob[CREDS_BLOB_LEN];
    size_t len = sizeof(blob);
    const esp_err_t get = nvs_get_blob(handle, CREDS_KEY, blob, &len);
    nvs_close(handle);
    if (get == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (get != ESP_OK) {
        return get;
    }
    if (len < 1 || (len - 1) % (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN) != 0) {
        ESP_LOGW(TAG, "corrupted pairing blob (%u), ignored", (unsigned)len);
        return ESP_OK;
    }
    const size_t count = (len - 1) / (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN);
    s_creds.count = count > NS2_CREDS_MAX ? NS2_CREDS_MAX : count;
    for (size_t i = 0; i < s_creds.count; i++) {
        const uint8_t *rec = &blob[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(s_creds.records[i].mac, rec, NS2_CREDS_MAC_LEN);
        memcpy(s_creds.records[i].ltk, rec + NS2_CREDS_MAC_LEN, NS2_CREDS_LTK_LEN);
    }
    ESP_LOGI(TAG, "loaded %u pairing record(s)", (unsigned)s_creds.count);
    return ESP_OK;
}

size_t ble_creds_count(void)
{
    size_t count = 0;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        count = s_creds.count;
        xSemaphoreGive(s_lock);
    }
    return count;
}

const ns2_cred_record_t *ble_creds_get(size_t index)
{
    if (index >= s_creds.count) {
        return NULL;
    }
    /* 记录槽仅在满员 memmove 时移动（五台主机场景），调用方读旧内容可接受。 */
    return &s_creds.records[index];
}

esp_err_t ble_creds_save(const uint8_t mac[NS2_CREDS_MAC_LEN],
                         const uint8_t ltk[NS2_CREDS_LTK_LEN])
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < s_creds.count; i++) {
        if (memcmp(s_creds.records[i].mac, mac, NS2_CREDS_MAC_LEN) == 0) {
            memcpy(s_creds.records[i].ltk, ltk, NS2_CREDS_LTK_LEN);
            uint8_t blob[CREDS_BLOB_LEN];
            serialize_locked(blob);
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "pairing record updated (total %u)", (unsigned)s_creds.count);
            xQueueSend(s_commit_queue, blob, portMAX_DELAY);
            return ESP_OK;
        }
    }
    if (s_creds.count >= NS2_CREDS_MAX) {
        /* 满员时覆盖最旧一条，保留最近配对的主机。 */
        memmove(&s_creds.records[0], &s_creds.records[1],
                sizeof(s_creds.records[0]) * (NS2_CREDS_MAX - 1));
        s_creds.count = NS2_CREDS_MAX - 1;
    }
    memcpy(s_creds.records[s_creds.count].mac, mac, NS2_CREDS_MAC_LEN);
    memcpy(s_creds.records[s_creds.count].ltk, ltk, NS2_CREDS_LTK_LEN);
    s_creds.count++;
    uint8_t blob[CREDS_BLOB_LEN];
    serialize_locked(blob);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "pairing record saved (total %u)", (unsigned)s_creds.count);
    xQueueSend(s_commit_queue, blob, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ble_creds_clear(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_creds.count = 0;
    uint8_t blob[CREDS_BLOB_LEN];
    serialize_locked(blob);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "pairing records cleared");
    xQueueSend(s_commit_queue, blob, portMAX_DELAY);
    return ESP_OK;
}

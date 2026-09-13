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
#define CREDS_KEY_V2 "pairing.v2"
#define CREDS_KEY_V1 "pairing"

/** v2 序列化格式：每个身份一段（1B 条数 + 最多 NS2_CREDS_MAX 条记录，
 * 每条 6B MAC + 16B LTK），三段顺排。v1 为单表（1B 条数 + 记录），仅读入
 * 迁移到 Pro 槽。 */
#define CREDS_SLOT_LEN (1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN))
#define CREDS_BLOB_LEN (NS2_ID_COUNT * CREDS_SLOT_LEN)
#define CREDS_COMMIT_QUEUE_LEN 4

static struct {
    ns2_cred_record_t records[NS2_ID_COUNT][NS2_CREDS_MAX];
    size_t count[NS2_ID_COUNT];
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
        esp_err_t set = nvs_set_blob(handle, CREDS_KEY_V2, blob, CREDS_BLOB_LEN);
        if (set == ESP_OK) {
            set = nvs_commit(handle);
        }
        nvs_close(handle);
        if (set != ESP_OK) {
            ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(set));
        } else {
            ESP_LOGI(TAG, "committed creds (pro=%u l=%u r=%u)",
                     blob[0], blob[CREDS_SLOT_LEN], blob[2 * CREDS_SLOT_LEN]);
        }
    }
}

/** 锁内序列化当前凭证表为固定长度快照。 */
static void serialize_locked(uint8_t blob[CREDS_BLOB_LEN])
{
    memset(blob, 0, CREDS_BLOB_LEN);
    for (size_t id = 0; id < NS2_ID_COUNT; id++) {
        uint8_t *slot = &blob[id * CREDS_SLOT_LEN];
        slot[0] = (uint8_t)s_creds.count[id];
        for (size_t i = 0; i < s_creds.count[id]; i++) {
            uint8_t *rec = &slot[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
            memcpy(rec, s_creds.records[id][i].mac, NS2_CREDS_MAC_LEN);
            memcpy(rec + NS2_CREDS_MAC_LEN, s_creds.records[id][i].ltk, NS2_CREDS_LTK_LEN);
        }
    }
}

/** 解析一段槽位到内存表。 */
static void load_slot_locked(ns2_identity_t identity, const uint8_t *slot)
{
    const size_t count = slot[0] > NS2_CREDS_MAX ? NS2_CREDS_MAX : slot[0];
    s_creds.count[identity] = count;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *rec = &slot[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(s_creds.records[identity][i].mac, rec, NS2_CREDS_MAC_LEN);
        memcpy(s_creds.records[identity][i].ltk, rec + NS2_CREDS_MAC_LEN, NS2_CREDS_LTK_LEN);
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
    memset(&s_creds, 0, sizeof(s_creds));

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
    const esp_err_t get = nvs_get_blob(handle, CREDS_KEY_V2, blob, &len);
    if (get == ESP_OK) {
        if (len < CREDS_BLOB_LEN) {
            ESP_LOGW(TAG, "corrupted creds blob (%u), ignored", (unsigned)len);
        } else {
            for (size_t id = 0; id < NS2_ID_COUNT; id++) {
                load_slot_locked((ns2_identity_t)id, &blob[id * CREDS_SLOT_LEN]);
            }
            ESP_LOGI(TAG, "loaded creds (pro=%u l=%u r=%u)",
                     (unsigned)s_creds.count[NS2_ID_PRO],
                     (unsigned)s_creds.count[NS2_ID_JOYCON_L],
                     (unsigned)s_creds.count[NS2_ID_JOYCON_R]);
            nvs_close(handle);
            return ESP_OK;
        }
    }
    /* v2 不存在或损坏：读 v1 单表迁移到 Pro 槽（保留既有配对）。 */
    len = CREDS_BLOB_LEN;
    const esp_err_t get_v1 = nvs_get_blob(handle, CREDS_KEY_V1, blob, &len);
    nvs_close(handle);
    if (get_v1 != ESP_OK || len < 1 ||
        (len - 1) % (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN) != 0) {
        return ESP_OK;
    }
    const size_t count = (len - 1) / (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN);
    s_creds.count[NS2_ID_PRO] = count > NS2_CREDS_MAX ? NS2_CREDS_MAX : count;
    for (size_t i = 0; i < s_creds.count[NS2_ID_PRO]; i++) {
        const uint8_t *rec = &blob[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(s_creds.records[NS2_ID_PRO][i].mac, rec, NS2_CREDS_MAC_LEN);
        memcpy(s_creds.records[NS2_ID_PRO][i].ltk, rec + NS2_CREDS_MAC_LEN, NS2_CREDS_LTK_LEN);
    }
    ESP_LOGI(TAG, "migrated %u legacy pairing record(s) -> pro",
             (unsigned)s_creds.count[NS2_ID_PRO]);
    return ESP_OK;
}

size_t ble_creds_count(ns2_identity_t identity)
{
    size_t count = 0;
    if (identity < NS2_ID_COUNT && s_lock != NULL &&
        xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        count = s_creds.count[identity];
        xSemaphoreGive(s_lock);
    }
    return count;
}

const ns2_cred_record_t *ble_creds_get(ns2_identity_t identity, size_t index)
{
    if (identity >= NS2_ID_COUNT || index >= s_creds.count[identity]) {
        return NULL;
    }
    /* 记录槽仅在满员 memmove 时移动（五台主机场景），调用方读旧内容可接受。 */
    return &s_creds.records[identity][index];
}

esp_err_t ble_creds_save(ns2_identity_t identity, const uint8_t mac[NS2_CREDS_MAC_LEN],
                         const uint8_t ltk[NS2_CREDS_LTK_LEN])
{
    if (identity >= NS2_ID_COUNT || s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < s_creds.count[identity]; i++) {
        if (memcmp(s_creds.records[identity][i].mac, mac, NS2_CREDS_MAC_LEN) == 0) {
            memcpy(s_creds.records[identity][i].ltk, ltk, NS2_CREDS_LTK_LEN);
            uint8_t blob[CREDS_BLOB_LEN];
            serialize_locked(blob);
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "pairing record updated (id=%u total %u)",
                     (unsigned)identity, (unsigned)s_creds.count[identity]);
            xQueueSend(s_commit_queue, blob, portMAX_DELAY);
            return ESP_OK;
        }
    }
    if (s_creds.count[identity] >= NS2_CREDS_MAX) {
        /* 满员时覆盖最旧一条，保留最近配对的主机。 */
        memmove(&s_creds.records[identity][0], &s_creds.records[identity][1],
                sizeof(s_creds.records[identity][0]) * (NS2_CREDS_MAX - 1));
        s_creds.count[identity] = NS2_CREDS_MAX - 1;
    }
    memcpy(s_creds.records[identity][s_creds.count[identity]].mac, mac, NS2_CREDS_MAC_LEN);
    memcpy(s_creds.records[identity][s_creds.count[identity]].ltk, ltk, NS2_CREDS_LTK_LEN);
    s_creds.count[identity]++;
    uint8_t blob[CREDS_BLOB_LEN];
    serialize_locked(blob);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "pairing record saved (id=%u total %u)",
             (unsigned)identity, (unsigned)s_creds.count[identity]);
    xQueueSend(s_commit_queue, blob, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ble_creds_clear(ns2_identity_t identity)
{
    if (identity >= NS2_ID_COUNT || s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_creds.count[identity] = 0;
    uint8_t blob[CREDS_BLOB_LEN];
    serialize_locked(blob);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "pairing records cleared (id=%u)", (unsigned)identity);
    xQueueSend(s_commit_queue, blob, portMAX_DELAY);
    return ESP_OK;
}

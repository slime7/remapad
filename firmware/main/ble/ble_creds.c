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
#define CREDS_KEY_HOST "pairing.host"

/** v2 序列化格式：每个身份一段（1B 条数 + 最多 NS2_CREDS_MAX 条记录，
 * 每条 6B MAC + 16B LTK），各段顺排。v1 为单表（1B 条数 + 记录），仅读入
 * 迁移到 Pro 槽。 */
#define CREDS_SLOT_LEN (1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN))
/** 落盘固定写满历史最长格式（三个身份段）：本设备只用第 0 段（Pro），余下补零。
 * 长度不变，新旧固件互相读回都不会把整张表当成损坏（读回按实际长度解析）。 */
#define CREDS_SLOTS_STORED 3
#define CREDS_BLOB_LEN (CREDS_SLOTS_STORED * CREDS_SLOT_LEN)
#define CREDS_HOST_LEN (CREDS_SLOTS_STORED * NS2_CREDS_MAC_LEN)
/** v1 单表的长度上限（1B 条数 + 最多 NS2_CREDS_MAX 条记录）。 */
#define CREDS_V1_LEN_MAX (1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN))
/** 读旧记录用的缓冲区上限：v2 与 v1 单表取大者。缓冲区必须够大，否则
 * nvs_get_blob 会因「缓冲不足」直接失败、已配对的凭证被当成没有
 * （2026-09-16 实机踩到：读回缓冲区按当前长度开、比盘上的短，整张表读不进来，
 * 会话回落去读 v1 老表，拿到的是过期记录）。 */
#define CREDS_BLOB_LEN_MAX \
    (CREDS_BLOB_LEN > CREDS_V1_LEN_MAX ? CREDS_BLOB_LEN : CREDS_V1_LEN_MAX)
#define CREDS_COMMIT_QUEUE_LEN 4

/** 落盘任务的消息：凭证表与主机地址表共用一条队列。 */
typedef struct {
    uint8_t kind; /* 0 = 凭证表，1 = 主机地址表 */
    uint8_t payload[CREDS_BLOB_LEN];
} creds_commit_t;

#define CREDS_COMMIT_CREDS 0
#define CREDS_COMMIT_HOST 1

static struct {
    ns2_cred_record_t records[NS2_ID_COUNT][NS2_CREDS_MAX];
    size_t count[NS2_ID_COUNT];
    uint8_t host_mac[NS2_ID_COUNT][NS2_CREDS_MAC_LEN];
    bool host_valid[NS2_ID_COUNT];
} s_creds;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commit_queue;

/** NVS 写任务：PocketJS owner task 的栈在 PSRAM，而 flash 写入期间缓存被
 * 禁用，此时访问 PSRAM（含任务自身的栈）会触发 cache 异常重启——「停止
 * 配对即重启」的根因。所有凭证落盘因此收敛到这个内部 RAM 栈的任务执行，
 * 调用方只更新内存表并投递快照，任何任务上下文都不会再写 flash。 */
static void commit_task(void *param)
{
    creds_commit_t msg;
    while (xQueueReceive(s_commit_queue, &msg, portMAX_DELAY) == pdTRUE) {
        nvs_handle_t handle;
        const esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
            continue;
        }
        esp_err_t set;
        if (msg.kind == CREDS_COMMIT_HOST) {
            set = nvs_set_blob(handle, CREDS_KEY_HOST, msg.payload, CREDS_HOST_LEN);
        } else {
            set = nvs_set_blob(handle, CREDS_KEY_V2, msg.payload, CREDS_BLOB_LEN);
        }
        if (set == ESP_OK) {
            set = nvs_commit(handle);
        }
        nvs_close(handle);
        if (set != ESP_OK) {
            ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(set));
        } else if (msg.kind == CREDS_COMMIT_CREDS) {
            ESP_LOGI(TAG, "committed creds (pro=%u)", msg.payload[0]);
        } else {
            ESP_LOGI(TAG, "committed host address %02x:%02x:%02x:%02x:%02x:%02x",
                     msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3],
                     msg.payload[4], msg.payload[5]);
        }
    }
}

/** 投递一条落盘消息（调用方持锁或不需要锁的初始化路径）。 */
static void enqueue_commit(uint8_t kind, const uint8_t *payload, size_t len)
{
    creds_commit_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = kind;
    memcpy(msg.payload, payload, len);
    xQueueSend(s_commit_queue, &msg, portMAX_DELAY);
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
    size_t count = slot[0] > NS2_CREDS_MAX ? NS2_CREDS_MAX : slot[0];
    /* 历史写入曾出现「计数 4、只有前 2 条有内容」的表：尾部全零记录不是有效
     * 凭证，按计数取最近一条会拿到全零主机地址，回连/唤醒广播因此作废。 */
    while (count > 0) {
        const uint8_t *rec = &slot[1 + (count - 1) * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        bool all_zero = true;
        for (size_t i = 0; i < NS2_CREDS_MAC_LEN; i++) {
            if (rec[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            break;
        }
        count--;
    }
    s_creds.count[identity] = count;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *rec = &slot[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(s_creds.records[identity][i].mac, rec, NS2_CREDS_MAC_LEN);
        memcpy(s_creds.records[identity][i].ltk, rec + NS2_CREDS_MAC_LEN, NS2_CREDS_LTK_LEN);
    }
}

/** 凭证里没有主机地址记录时，用最近一条有效凭证兜底（老设备升级路径）。 */
static void seed_host_from_creds(void)
{
    for (size_t id = 0; id < NS2_ID_COUNT; id++) {
        if (!s_creds.host_valid[id] && s_creds.count[id] > 0) {
            memcpy(s_creds.host_mac[id], s_creds.records[id][s_creds.count[id] - 1].mac,
                   NS2_CREDS_MAC_LEN);
            s_creds.host_valid[id] = true;
        }
    }
}

esp_err_t ble_creds_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_commit_queue = xQueueCreate(CREDS_COMMIT_QUEUE_LEN, sizeof(creds_commit_t));
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
    /* 缓冲区按历史最长格式开，读回时按实际长度只解析第一段（Pro 槽）。 */
    uint8_t blob[CREDS_BLOB_LEN_MAX];
    size_t len = sizeof(blob);
    const esp_err_t get = nvs_get_blob(handle, CREDS_KEY_V2, blob, &len);
    if (get == ESP_OK && len >= CREDS_SLOT_LEN) {
        load_slot_locked(NS2_ID_PRO, blob);
        ESP_LOGI(TAG, "loaded creds (pro=%u, blob %uB)",
                 (unsigned)s_creds.count[NS2_ID_PRO], (unsigned)len);
    } else {
        if (get == ESP_OK) {
            ESP_LOGW(TAG, "corrupted creds blob (%u), ignored", (unsigned)len);
        }
        /* v2 不存在或损坏：读 v1 单表迁移到 Pro 槽（保留既有配对）。 */
        len = sizeof(blob);
        const esp_err_t get_v1 = nvs_get_blob(handle, CREDS_KEY_V1, blob, &len);
        if (get_v1 == ESP_OK && len >= 1 &&
            (len - 1) % (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN) == 0) {
            const size_t count = (len - 1) / (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN);
            s_creds.count[NS2_ID_PRO] = count > NS2_CREDS_MAX ? NS2_CREDS_MAX : count;
            for (size_t i = 0; i < s_creds.count[NS2_ID_PRO]; i++) {
                const uint8_t *rec = &blob[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
                memcpy(s_creds.records[NS2_ID_PRO][i].mac, rec, NS2_CREDS_MAC_LEN);
                memcpy(s_creds.records[NS2_ID_PRO][i].ltk, rec + NS2_CREDS_MAC_LEN,
                       NS2_CREDS_LTK_LEN);
            }
            ESP_LOGI(TAG, "migrated %u legacy pairing record(s) -> pro",
                     (unsigned)s_creds.count[NS2_ID_PRO]);
        }
    }
    seed_host_from_creds();
    /* 连接时记录的主机地址优先于凭证推断值：它一定是主机当前在用的地址。 */
    uint8_t host[CREDS_HOST_LEN];
    len = sizeof(host);
    /* 长度下限只按第一条记录算：盘上可能是历史上的短格式，前 6 字节总是 Pro 的。 */
    if (nvs_get_blob(handle, CREDS_KEY_HOST, host, &len) == ESP_OK &&
        len >= NS2_CREDS_MAC_LEN) {
        memcpy(s_creds.host_mac[NS2_ID_PRO], host, NS2_CREDS_MAC_LEN);
        s_creds.host_valid[NS2_ID_PRO] = true;
        ESP_LOGI(TAG, "loaded host address %02x:%02x:%02x:%02x:%02x:%02x",
                 host[0], host[1], host[2], host[3], host[4], host[5]);
    }
    nvs_close(handle);
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
            enqueue_commit(CREDS_COMMIT_CREDS, blob, CREDS_BLOB_LEN);
            return ESP_OK;
        }
    }
    if (s_creds.count[identity] >= NS2_CREDS_KEEP) {
        /* 超过保留条数时丢弃最旧一条（回连广播取最近一条）。 */
        const size_t drop = s_creds.count[identity] - NS2_CREDS_KEEP + 1;
        memmove(&s_creds.records[identity][0], &s_creds.records[identity][drop],
                sizeof(s_creds.records[identity][0]) * (s_creds.count[identity] - drop));
        s_creds.count[identity] -= drop;
    }
    memcpy(s_creds.records[identity][s_creds.count[identity]].mac, mac, NS2_CREDS_MAC_LEN);
    memcpy(s_creds.records[identity][s_creds.count[identity]].ltk, ltk, NS2_CREDS_LTK_LEN);
    s_creds.count[identity]++;
    uint8_t blob[CREDS_BLOB_LEN];
    serialize_locked(blob);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "pairing record saved (id=%u total %u)",
             (unsigned)identity, (unsigned)s_creds.count[identity]);
    enqueue_commit(CREDS_COMMIT_CREDS, blob, CREDS_BLOB_LEN);
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
    enqueue_commit(CREDS_COMMIT_CREDS, blob, CREDS_BLOB_LEN);
    return ESP_OK;
}

void ble_creds_note_host_mac(ns2_identity_t identity, const uint8_t mac[NS2_CREDS_MAC_LEN])
{
    if (identity >= NS2_ID_COUNT || mac == NULL || s_lock == NULL) {
        return;
    }
    bool all_zero = true;
    for (size_t i = 0; i < NS2_CREDS_MAC_LEN; i++) {
        if (mac[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool same = s_creds.host_valid[identity] &&
                      memcmp(s_creds.host_mac[identity], mac, NS2_CREDS_MAC_LEN) == 0;
    if (!same) {
        memcpy(s_creds.host_mac[identity], mac, NS2_CREDS_MAC_LEN);
        s_creds.host_valid[identity] = true;
        uint8_t host[CREDS_HOST_LEN];
        /* 盘上按历史长度（三身份段）写；本设备只有第 0 段有效，其余补零。 */
        memset(host, 0, sizeof(host));
        memcpy(host, s_creds.host_mac[identity], NS2_CREDS_MAC_LEN);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "host address noted (id=%u %02x:%02x:%02x:%02x:%02x:%02x)",
                 (unsigned)identity, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        enqueue_commit(CREDS_COMMIT_HOST, host, CREDS_HOST_LEN);
        return;
    }
    xSemaphoreGive(s_lock);
}

bool ble_creds_host_mac(ns2_identity_t identity, uint8_t out[NS2_CREDS_MAC_LEN])
{
    if (identity >= NS2_ID_COUNT || out == NULL || s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool valid = s_creds.host_valid[identity];
    if (valid) {
        memcpy(out, s_creds.host_mac[identity], NS2_CREDS_MAC_LEN);
    }
    xSemaphoreGive(s_lock);
    return valid;
}

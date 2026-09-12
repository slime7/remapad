#include "ble_creds.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "remapad_blcred";

#define CREDS_NS "remapad"
#define CREDS_KEY "pairing"

static struct {
    ns2_cred_record_t records[NS2_CREDS_MAX];
    size_t count;
} s_creds;

esp_err_t ble_creds_init(void)
{
    s_creds.count = 0;
    nvs_handle_t handle;
    const esp_err_t err = nvs_open(CREDS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t blob[1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
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

static esp_err_t creds_commit(void)
{
    uint8_t blob[1 + NS2_CREDS_MAX * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
    blob[0] = (uint8_t)s_creds.count;
    for (size_t i = 0; i < s_creds.count; i++) {
        uint8_t *rec = &blob[1 + i * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN)];
        memcpy(rec, s_creds.records[i].mac, NS2_CREDS_MAC_LEN);
        memcpy(rec + NS2_CREDS_MAC_LEN, s_creds.records[i].ltk, NS2_CREDS_LTK_LEN);
    }
    nvs_handle_t handle;
    const esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    const size_t len = 1 + s_creds.count * (NS2_CREDS_MAC_LEN + NS2_CREDS_LTK_LEN);
    esp_err_t set = nvs_set_blob(handle, CREDS_KEY, blob, len);
    if (set == ESP_OK) {
        set = nvs_commit(handle);
    }
    nvs_close(handle);
    if (set != ESP_OK) {
        ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(set));
    }
    return set;
}

size_t ble_creds_count(void)
{
    return s_creds.count;
}

const ns2_cred_record_t *ble_creds_get(size_t index)
{
    if (index >= s_creds.count) {
        return NULL;
    }
    return &s_creds.records[index];
}

esp_err_t ble_creds_save(const uint8_t mac[NS2_CREDS_MAC_LEN],
                         const uint8_t ltk[NS2_CREDS_LTK_LEN])
{
    for (size_t i = 0; i < s_creds.count; i++) {
        if (memcmp(s_creds.records[i].mac, mac, NS2_CREDS_MAC_LEN) == 0) {
            memcpy(s_creds.records[i].ltk, ltk, NS2_CREDS_LTK_LEN);
            return creds_commit();
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
    ESP_LOGI(TAG, "pairing record saved (total %u)", (unsigned)s_creds.count);
    return creds_commit();
}

esp_err_t ble_creds_clear(void)
{
    s_creds.count = 0;
    ESP_LOGI(TAG, "pairing records cleared");
    return creds_commit();
}

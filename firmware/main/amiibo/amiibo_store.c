#include "amiibo_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ns2_nfc.h"

static const char *TAG = "remapad_amiibo";

/** storage 分区挂载点与文件命名：镜像 "a<i>"、名称 "n<i>"、选中号 "sel"。 */
#define AMIIBO_FS_BASE "/amiibo"
#define AMIIBO_FS_PARTITION "storage"
#define AMIIBO_SEL_NONE 0xFFu

static struct {
    bool mounted;
    bool used[AMIIBO_SLOTS_MAX];
    int selected;
} s_store;

static void slot_path(char *out, size_t cap, const char kind, size_t index)
{
    snprintf(out, cap, AMIIBO_FS_BASE "/%c%03u", kind, (unsigned)index);
}

/** 写整个文件（新建/截断），失败返回 false。 */
static bool write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len && fclose(f) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "write %s failed", path);
    }
    return ok;
}

static void save_selected(int index)
{
    const uint8_t value = index < 0 ? AMIIBO_SEL_NONE : (uint8_t)index;
    if (!write_file(AMIIBO_FS_BASE "/sel", &value, sizeof(value))) {
        ESP_LOGW(TAG, "persist selection %d failed", index);
    }
}

esp_err_t amiibo_store_init(void)
{
    if (s_store.mounted) {
        return ESP_OK;
    }
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = AMIIBO_FS_BASE,
        .partition_label = AMIIBO_FS_PARTITION,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount '" AMIIBO_FS_PARTITION "' failed: %s", esp_err_to_name(err));
        return err;
    }
    s_store.mounted = true;

    /* 单次目录遍历代替逐槽 stat：未命中的 stat 每次都要走完整目录查找，
     * 200 槽扫下来是秒级，readdir 一遍只花毫秒（开机黑屏的元凶之一）。 */
    memset(s_store.used, 0, sizeof(s_store.used));
    s_store.selected = -1;
    DIR *dir = opendir(AMIIBO_FS_BASE);
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] != 'a' || strlen(ent->d_name) != 4) {
                continue;
            }
            char *end = NULL;
            const unsigned long index = strtoul(&ent->d_name[1], &end, 10);
            if (end == NULL || *end != '\0' || index >= AMIIBO_SLOTS_MAX) {
                continue;
            }
            char path[24];
            slot_path(path, sizeof(path), 'a', (size_t)index);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size >= (off_t)NS2_NFC_TAG_SIZE) {
                s_store.used[index] = true;
            }
        }
        closedir(dir);
    }
    FILE *sel = fopen(AMIIBO_FS_BASE "/sel", "rb");
    if (sel != NULL) {
        uint8_t value = AMIIBO_SEL_NONE;
        const bool got = fread(&value, 1, sizeof(value), sel) == sizeof(value);
        fclose(sel);
        if (got && value < AMIIBO_SLOTS_MAX && s_store.used[value]) {
            uint8_t record[AMIIBO_RECORD_SIZE];
            const size_t len = amiibo_store_read_record(value, record, sizeof(record));
            if (len > 0 && ns2_nfc_stage(record, len) == ESP_OK) {
                s_store.selected = (int)value;
                char name[AMIIBO_NAME_MAX + 1] = "?";
                (void)amiibo_store_name(value, name, sizeof(name));
                ESP_LOGI(TAG, "selection restored: slot %u '%s'", (unsigned)value, name);
            }
        }
    }
    /* 主机写卡的存档写回当前选中槽位（没有选中就只更新内存镜像）。 */
    ns2_nfc_set_write_sink(amiibo_store_write_selected, NULL);
    ESP_LOGI(TAG, "store ready on '" AMIIBO_FS_PARTITION "' (%u/%u slots)",
             (unsigned)amiibo_store_count(), (unsigned)amiibo_store_capacity());
    return ESP_OK;
}

void amiibo_store_init_task(void *unused)
{
    (void)unused;
    if (amiibo_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "store init failed (tag emulation stays empty)");
    }
    vTaskDelete(NULL);
}

size_t amiibo_store_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < AMIIBO_SLOTS_MAX; i++) {
        n += s_store.used[i] ? 1u : 0u;
    }
    return n;
}

size_t amiibo_store_capacity(void)
{
    return AMIIBO_SLOTS_MAX;
}

bool amiibo_store_name(size_t index, char *out, size_t cap)
{
    if (!s_store.mounted || index >= AMIIBO_SLOTS_MAX || !s_store.used[index] ||
        out == NULL || cap == 0) {
        return false;
    }
    char path[24];
    slot_path(path, sizeof(path), 'n', index);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    const size_t len = fread(out, 1, cap - 1, f);
    fclose(f);
    out[len] = 0;
    return len > 0;
}

size_t amiibo_store_read_record(size_t index, uint8_t *out, size_t cap)
{
    if (!s_store.mounted || index >= AMIIBO_SLOTS_MAX || !s_store.used[index] ||
        out == NULL || cap < AMIIBO_RECORD_SIZE) {
        return 0;
    }
    char path[24];
    slot_path(path, sizeof(path), 'a', index);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    const size_t len = fread(out, 1, AMIIBO_RECORD_SIZE, f);
    fclose(f);
    return len >= NS2_NFC_TAG_SIZE ? len : 0;
}

bool amiibo_store_read(size_t index, uint8_t *out, size_t cap)
{
    return amiibo_store_read_record(index, out, cap) != 0;
}

int amiibo_store_add(const char *name, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!s_store.mounted || name == NULL || name[0] == 0 || data == NULL ||
        (len != NS2_NFC_TAG_SIZE && len != AMIIBO_RECORD_SIZE)) {
        return -1;
    }
    int slot = -1;
    for (size_t i = 0; i < AMIIBO_SLOTS_MAX; i++) {
        if (!s_store.used[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no free slot for '%s'", name);
        return -1;
    }
    uint8_t record[AMIIBO_RECORD_SIZE];
    memcpy(record, data, len);
    if (len < sizeof(record)) {
        memset(&record[len], 0, sizeof(record) - len);
    }
    char path[24];
    slot_path(path, sizeof(path), 'a', (size_t)slot);
    if (!write_file(path, record, sizeof(record))) {
        return -1;
    }
    slot_path(path, sizeof(path), 'n', (size_t)slot);
    if (!write_file(path, name, strlen(name))) {
        slot_path(path, sizeof(path), 'a', (size_t)slot);
        unlink(path);
        return -1;
    }
    s_store.used[slot] = true;
    ESP_LOGI(TAG, "stored '%s' as slot %d (%u used)", name, slot, amiibo_store_count());
    return slot;
}

esp_err_t amiibo_store_remove(size_t index)
{
    if (!s_store.mounted || index >= AMIIBO_SLOTS_MAX || !s_store.used[index]) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[24];
    slot_path(path, sizeof(path), 'a', index);
    unlink(path);
    slot_path(path, sizeof(path), 'n', index);
    unlink(path);
    s_store.used[index] = false;
    ESP_LOGI(TAG, "removed slot %u (%u used)", (unsigned)index, amiibo_store_count());
    if (s_store.selected == (int)index) {
        s_store.selected = -1;
        save_selected(-1);
        ns2_nfc_stage(NULL, 0);
    }
    return ESP_OK;
}

esp_err_t amiibo_store_select(size_t index)
{
    if (!s_store.mounted || index >= AMIIBO_SLOTS_MAX || !s_store.used[index]) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t record[AMIIBO_RECORD_SIZE];
    const size_t len = amiibo_store_read_record(index, record, sizeof(record));
    if (len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t staged = ns2_nfc_stage(record, len);
    if (staged != ESP_OK) {
        return staged;
    }
    s_store.selected = (int)index;
    save_selected((int)index);
    char name[AMIIBO_NAME_MAX + 1] = "?";
    (void)amiibo_store_name(index, name, sizeof(name));
    ESP_LOGI(TAG, "selected slot %u '%s' (nfc state 0x%02x)", (unsigned)index, name,
             ns2_nfc_report_state());
    return ESP_OK;
}

void amiibo_store_deselect(void)
{
    if (s_store.selected < 0) {
        return;
    }
    s_store.selected = -1;
    ns2_nfc_stage(NULL, 0);
    save_selected(-1);
    ESP_LOGI(TAG, "deselected (nfc state idle)");
}

int amiibo_store_selected(void)
{
    return s_store.selected;
}

bool amiibo_store_write_selected(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!s_store.mounted || s_store.selected < 0 || data == NULL ||
        len != NS2_NFC_TAG_SIZE) {
        return false;
    }
    uint8_t record[AMIIBO_RECORD_SIZE];
    if (amiibo_store_read_record((size_t)s_store.selected, record, sizeof(record)) == 0) {
        return false;
    }
    memcpy(record, data, NS2_NFC_TAG_SIZE); /* 签名段保持原值。 */
    char path[24];
    slot_path(path, sizeof(path), 'a', (size_t)s_store.selected);
    if (!write_file(path, record, sizeof(record))) {
        return false;
    }
    ESP_LOGI(TAG, "game save written back to slot %d", s_store.selected);
    return true;
}

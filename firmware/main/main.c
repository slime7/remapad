#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app_pocket.h"

static const char *TAG = "remapad_app";

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "Remapad ESP32-S3 N16R8 启动中...");
    ESP_LOGI(TAG, "=================================");

    // 打印芯片与内存状态
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "内部 SRAM 空闲: %u 字节", (unsigned int)internal_free);
    ESP_LOGI(TAG, "Octal PSRAM 空闲: %u 字节 (约 %.2f MB)",
             (unsigned int)psram_free, (double)psram_free / (1024.0 * 1024.0));

    if (psram_free == 0) {
        ESP_LOGE(TAG, "警告: 未检测到 PSRAM 或 PSRAM 初始化失败，请检查 sdkconfig 中的 SPIRAM 配置！");
    } else {
        ESP_LOGI(TAG, "PSRAM 初始化正常，满足 PocketJS 运行要求。");
    }

    // PocketJS 应用资产包自检
    bool magic_ok = (app_pocket_len >= 16) &&
                    (app_pocket_data[0] == 'P' &&
                     app_pocket_data[1] == 'C' &&
                     app_pocket_data[2] == 'K' &&
                     app_pocket_data[3] == 'T');

    if (magic_ok) {
        uint32_t version = *(const uint32_t *)&app_pocket_data[4];
        uint32_t manifest_len = *(const uint32_t *)&app_pocket_data[8];
        uint32_t variant_count = *(const uint32_t *)&app_pocket_data[12];
        ESP_LOGI(TAG, "PocketJS 包加载成功: 大小 %u 字节, 版本 v%" PRIu32 ", 清单长度 %" PRIu32 ", 目标变体数 %" PRIu32,
                 (unsigned int)app_pocket_len, version, manifest_len, variant_count);
    } else {
        ESP_LOGW(TAG, "未检测到有效的 PocketJS (PCKT) 资源包");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

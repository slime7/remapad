#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "pocketjs_host.h"

void app_main(void)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("remapad_app", "Remapad ESP32-S3 PocketJS host starting");
    ESP_LOGI("remapad_app", "Internal SRAM free: %" PRIu32 " bytes", (uint32_t)internal_free);
    ESP_LOGI("remapad_app", "PSRAM free: %" PRIu32 " bytes", (uint32_t)psram_free);

    ESP_ERROR_CHECK(remapad_pocketjs_start());
    ESP_LOGI("remapad_app", "PocketJS runner started");
}

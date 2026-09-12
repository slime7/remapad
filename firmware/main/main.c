#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "dp_plane.h"
#include "pocketjs_host.h"

/** NVS 存放 PHY 校准与（M3 起）BLE 配对凭证；擦除恢复仅发生在介质损坏场景。 */
static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("remapad_app", "nvs needs recovery, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    /* 广播地址伪装必须在蓝牙控制器初始化前完成：public 广播的空中地址
     * 由 controller 的 BD_ADDR 决定，host 侧改不动。Switch 2 主机在芯片层
     * 过滤广播并匹配地址 OUI，故将 base MAC 换成 Nintendo OUI（98:E2:55，
     * 实机抓包），蓝牙地址（base+2）随之派生；后缀沿用 eFuse，上电稳定。 */
    uint8_t base[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_read_mac(base, ESP_MAC_WIFI_STA);
    base[0] = 0x98;
    base[1] = 0xE2;
    base[2] = 0x55;
    ESP_ERROR_CHECK(esp_base_mac_addr_set(base));

    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("remapad_app", "Remapad ESP32-S3 PocketJS host starting");
    ESP_LOGI("remapad_app", "Internal SRAM free: %" PRIu32 " bytes", (uint32_t)internal_free);
    ESP_LOGI("remapad_app", "PSRAM free: %" PRIu32 " bytes", (uint32_t)psram_free);

    nvs_init();
    ESP_ERROR_CHECK(remapad_pocketjs_start());
    ESP_LOGI("remapad_app", "PocketJS owner task started");

    /* 数据面失败不阻断屏幕 UI 启动。 */
    const esp_err_t dp_err = dp_plane_start();
    if (dp_err != ESP_OK) {
        ESP_LOGE("remapad_app", "data plane start failed: %s", esp_err_to_name(dp_err));
    }
}

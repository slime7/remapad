#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "bridge/js_bridge.h"
#include "console/cli.h"
#include "dp_plane.h"
#include "drivers/buzzer.h"
#include "drivers/pwr_key.h"
#include "pocketjs_host.h"

/** NVS 存放 PHY 校准、BLE 配对凭证与用户设置；擦除恢复仅发生在介质损坏场景。 */
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

/** PWR 按键事件（pwr-key 任务上下文）：短按息屏/亮屏，长按 3-6s 切换
 *  连接模式（device ↔ host，桥接锁定；经 bridge 外部队列走持久化路径）。 */
static void pwr_key_handler(pwr_key_event_t event, void *user)
{
    (void)user;
    if (event == PWR_KEY_SHORT) {
        js_bridge_screen_power(!app_config_get()->screen_on);
        return;
    }
    const bool to_host = app_config_get()->usb_role != APP_CONFIG_USB_HOST;
    char json[64];
    snprintf(json, sizeof(json), "{\"t\":\"setUsbRole\",\"role\":\"%s\",\"id\":0}",
             to_host ? "host" : "device");
    js_bridge_submit_command(json);
}

void app_main(void)
{
    /* 广播地址伪装必须在蓝牙控制器初始化前完成：public 广播的空中地址
     * 由 controller 的 BD_ADDR 决定，host 侧改不动。实测主机不校验地址
     * OUI（配对与回连均成功），但为与已验证实现保持一致，沿用任天堂
     * 78:81:8C OUI；蓝牙地址（base+2）随之派生，后缀沿用 eFuse，上电稳定。 */
    uint8_t base[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_read_mac(base, ESP_MAC_WIFI_STA);
    base[0] = 0x78;
    base[1] = 0x81;
    base[2] = 0x8C;
    ESP_ERROR_CHECK(esp_base_mac_addr_set(base));

    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("remapad_app", "Remapad ESP32-S3 PocketJS host starting");
    ESP_LOGI("remapad_app", "Internal SRAM free: %" PRIu32 " bytes", (uint32_t)internal_free);
    ESP_LOGI("remapad_app", "PSRAM free: %" PRIu32 " bytes", (uint32_t)psram_free);

    nvs_init();
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(remapad_pocketjs_start());
    ESP_LOGI("remapad_app", "PocketJS owner task started");

    /* 蜂鸣器供 PWR 长按提示使用；初始化失败只影响提示音，不阻断启动。 */
    if (buzzer_init() != ESP_OK) {
        ESP_LOGE("remapad_app", "buzzer init failed");
    }

    /* 数据面失败不阻断屏幕 UI 启动。 */
    const esp_err_t dp_err = dp_plane_start();
    if (dp_err != ESP_OK) {
        ESP_LOGE("remapad_app", "data plane start failed: %s", esp_err_to_name(dp_err));
    }

    /* 串口 CLI 与 PWR 按键失败不阻断启动（记日志即可）。 */
    const esp_err_t cli_err = remapad_cli_start();
    if (cli_err != ESP_OK) {
        ESP_LOGE("remapad_app", "cli start failed: %s", esp_err_to_name(cli_err));
    }
    const esp_err_t pwr_err = pwr_key_start(pwr_key_handler, NULL);
    if (pwr_err != ESP_OK) {
        ESP_LOGE("remapad_app", "pwr key start failed: %s", esp_err_to_name(pwr_err));
    }
}

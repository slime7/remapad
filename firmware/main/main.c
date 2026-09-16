#include <inttypes.h>
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
#include "input_link.h"
#include "ota_session.h"
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

/** PWR 按键事件（pwr-key 任务上下文）：短按息屏/亮屏，长按 3-6s 是连接键
 *  ——有链路或正在广播就断开并静默，否则打开连接（已配对身份回连形态、
 *  未配对身份发现广播）。命令经 bridge 外部队列在 owner task 上执行。 */
static void pwr_key_handler(pwr_key_event_t event, void *user)
{
    (void)user;
    if (event == PWR_KEY_SHORT) {
        js_bridge_screen_power(!app_config_get()->screen_on);
        return;
    }
    js_bridge_connect_key();
}

void app_main(void)
{
    /* 电源保持必须在最前面：电池供电时松开 PWR 键就靠这一脚维持供电，
     * 放到外设初始化之后会让上电窗口白白拉长。失败不阻断启动：USB 供电
     * 下锁存被旁路，屏与 UI 仍能起来，只有电池供电会掉电。 */
    if (pwr_key_power_hold() != ESP_OK) {
        ESP_LOGE("remapad_app", "power latch not held, battery power will drop");
    }

    /* 广播地址伪装必须在蓝牙控制器初始化前完成：public 广播的空中地址
     * 由 controller 的 BD_ADDR 决定，host 侧改不动。实测主机不校验地址
     * OUI（99:E2:55 与 00:11:22 都能被搜索、配对，见 controller.md §12）。
     * 这里换成一个主机没见过的 OUI：主机按地址存配对记录，旧地址上那份
     * 记录（以及随之作废的 LTK）会让回连停在加密失败上，换地址等于让它把
     * 本设备当新设备重配一次。蓝牙地址（base+2）随之派生，后缀沿用 eFuse，
     * 上电稳定。 */
    uint8_t base[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_read_mac(base, ESP_MAC_WIFI_STA);
    base[0] = 0x9C;
    base[1] = 0xE6;
    base[2] = 0x35;
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
    /* OTA 升级通道先于桥接链路起来：input_link 读到 OTA 帧时分派给它。
     * 失败不阻断启动，屏幕 UI 与 BLE 链路照常工作。 */
    const esp_err_t ota_err = ota_session_start();
    if (ota_err != ESP_OK) {
        ESP_LOGE("remapad_app", "ota session start failed: %s", esp_err_to_name(ota_err));
    }
    /* 桥接链路接管 USJ 读取：安装驱动、把非帧字节转给 CLI。失败不阻断启动，
     * 屏幕 UI 与 BLE 链路照常工作。 */
    const esp_err_t link_err = input_link_start();
    if (link_err != ESP_OK) {
        ESP_LOGE("remapad_app", "bridge link start failed: %s", esp_err_to_name(link_err));
    }
    const esp_err_t pwr_err = pwr_key_start(pwr_key_handler, NULL);
    if (pwr_err != ESP_OK) {
        ESP_LOGE("remapad_app", "pwr key start failed: %s", esp_err_to_name(pwr_err));
    }
}

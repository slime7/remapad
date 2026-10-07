#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "amiibo_store.h"
#include "app_config.h"
#include "bridge/js_bridge.h"
#include "console/cli.h"
#include "console_out.h"
#include "dp_plane.h"
#include "drivers/buzzer.h"
#include "drivers/pwr_key.h"
#include "input_link.h"
#include "ota_link.h"
#include "ota_session.h"
#include "ui_service.h"

/** 完全关机状态重新上电的开机提示音时长（毫秒）：与 PWR 长按提示同为一声短鸣。 */
#define REMAPAD_POWER_ON_BEEP_MS 120

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
 *  未配对身份发现广播）。命令经 bridge 外部队列在控制面服务任务上执行。 */
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

  /* 日志出口全程归控制台通道管：device/host 两态都能被 netlog 汇点抄到；
   * 未注册汇点时与默认 vprintf 行为一致。 */
  console_out_init();

  /* 广播地址伪装必须在蓝牙控制器初始化前完成：public 广播的空中地址由 controller 的 BD_ADDR 决定。
     * 仅覆写蓝牙接口的 MAC（ESP_MAC_BT），前 3 字节换成任天堂 OUI，后缀沿用 eFuse；
     * 绝不能调 esp_base_mac_addr_set 污染全局基准 MAC，否则 WiFi STA 也会变成任天堂 OUI，
     * 导致大部分无线路由器与手机热点直接判定为异常伪造源并不予应答。 */
  uint8_t bt_mac[6] = { 0 };
  ESP_ERROR_CHECK(esp_read_mac(bt_mac, ESP_MAC_BT));
  bt_mac[0] = 0x9C;
  bt_mac[1] = 0xE6;
  bt_mac[2] = 0x35;
  ESP_ERROR_CHECK(esp_iface_mac_addr_set(bt_mac, ESP_MAC_BT));

  const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  ESP_LOGI("remapad_app", "Remapad ESP32-S3 UI host starting");
  ESP_LOGI("remapad_app", "Internal SRAM free: %" PRIu32 " bytes", (uint32_t)internal_free);
  ESP_LOGI("remapad_app", "PSRAM free: %" PRIu32 " bytes", (uint32_t)psram_free);

  nvs_init();
  ESP_ERROR_CHECK(app_config_init());
  if (xTaskCreate(amiibo_store_init_task, "amiibo-init", 8192, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE("remapad_app", "amiibo init task create failed (tag emulation stays empty)");
  }
  /* 控制面先行：命令队列与服务任务是无 UI 构建也必须活着的通路。 */
  ESP_ERROR_CHECK(js_bridge_init());
  const esp_err_t bridge_err = js_bridge_service_start();
  if (bridge_err != ESP_OK) {
    ESP_LOGE("remapad_app", "bridge service start failed: %s", esp_err_to_name(bridge_err));
  }
  /* 屏幕 UI 是可选装配：契约见 ui_service.h，提供者是界面组件或无 UI 空实现。 */
  ESP_ERROR_CHECK(remapad_ui_start());
  ESP_LOGI("remapad_app", "screen UI provider started");

  /* 蜂鸣器供 PWR 长按提示与开机提示音使用；初始化失败只影响提示音，不阻断启动。 */
  if (buzzer_init() != ESP_OK) {
    ESP_LOGE("remapad_app", "buzzer init failed");
  }
  /* 完全关机状态重新上电（复位原因 power-on：按 PWR 或插上 USB 都算）时短鸣
     * 一声作开机反馈；软件复位、OTA 重启与看门狗复位不响，不把固件自身重启当开机。 */
  if (esp_reset_reason() == ESP_RST_POWERON) {
    buzzer_beep(REMAPAD_POWER_ON_BEEP_MS);
    ESP_LOGI("remapad_app", "power-on beep");
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
  /* OTA 升级通道先于桥接链路起来：input_link 读到 OTA 帧时经 ota_link 适配分派给会话核心。
     * 失败不阻断启动，屏幕 UI 与 BLE 链路照常工作。 */
  const esp_err_t ota_err = ota_link_start();
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
  /* 装配收尾即向 OTA 回滚门槛报就绪：判据是核心服务启动完成 + 开机满 30 秒，
     * 不看画面首帧，有屏与无屏构建同一口径。 */
  ota_session_notify_ready();
}

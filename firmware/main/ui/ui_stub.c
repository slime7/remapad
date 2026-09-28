/**
 * 无 UI 构建（REMAPAD_UI=OFF）的界面提供者空实现：面板/触摸/背光不初始化、
 * 屏幕保持熄灭，设置只经 PC 串口 CLI 控制。
 */
#include "ui_service.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "remapad_ui";

esp_err_t remapad_ui_start(void)
{
  ESP_LOGI(TAG, "no-UI build: screen stays dark, control via serial CLI");
  return ESP_OK;
}

void remapad_ui_request_shot(void)
{
  ESP_LOGW(TAG, "shot unavailable in no-UI build");
}

void remapad_ui_request_trace(unsigned frames)
{
  (void)frames;
  ESP_LOGW(TAG, "trace unavailable in no-UI build");
}

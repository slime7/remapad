#include "js_bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "../drivers/backlight.h"
#include "../drivers/battery.h"

static const char *TAG = "js_bridge";

esp_err_t js_bridge_init(void)
{
    ESP_LOGI(TAG, "初始化硬件调用桥接调度通道");
    backlight_init();
    battery_init();
    return ESP_OK;
}

void js_bridge_handle_cmd(const char *cmd_json)
{
    if (cmd_json == NULL) {
        return;
    }

    ESP_LOGI(TAG, "收到前端指令: %s", cmd_json);

    if (strstr(cmd_json, "\"setBacklight\"") != NULL) {
        const char *val_ptr = strstr(cmd_json, "\"brightness\":");
        uint8_t brightness = 80;
        if (val_ptr != NULL) {
            brightness = (uint8_t)atoi(val_ptr + 13);
        }
        backlight_set(brightness);
        ESP_LOGI(TAG, "已响应背光设置: %u%%", brightness);
    } else if (strstr(cmd_json, "\"triggerRumble\"") != NULL) {
        ESP_LOGI(TAG, "已响应 Switch 2 HD 震动测试指令");
    } else if (strstr(cmd_json, "\"getSystemStatus\"") != NULL) {
        ESP_LOGI(TAG, "已响应系统状态查询指令");
    }
}

void js_bridge_post_event(const char *event_json)
{
    if (event_json == NULL) {
        return;
    }
    ESP_LOGI(TAG, "向前端推送事件: %s", event_json);
}

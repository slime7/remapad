/**
 * app_config 的测试替身：主机测试不引入 NVS，内存表即配置。
 * ns2_frames 只读 fw_version，ui_service 读写配色；setter 直接改内存表。
 */
#include "app_config.h"

#include <string.h>

static app_config_t s_config;

const app_config_t *app_config_get(void)
{
    return &s_config;
}

void app_config_set_controller_colors(uint32_t body_rgb, uint32_t button_rgb,
                                      uint32_t accent_rgb, uint32_t grip_rgb)
{
    s_config.body_color = body_rgb;
    s_config.button_color = button_rgb;
    s_config.accent_color = accent_rgb;
    s_config.grip_color = grip_rgb;
}

void host_test_set_app_config(const app_config_t *config)
{
    s_config = *config;
}

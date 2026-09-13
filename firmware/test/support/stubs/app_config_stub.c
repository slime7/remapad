/**
 * app_config 的测试替身：ns2_frames 只读 fw_version，这里给一份固定配置。
 * 真机上由 NVS 装载，主机测试不引入 NVS。
 */
#include "app_config.h"

#include <string.h>

static app_config_t s_config;

const app_config_t *app_config_get(void)
{
    return &s_config;
}

void host_test_set_app_config(const app_config_t *config)
{
    s_config = *config;
}


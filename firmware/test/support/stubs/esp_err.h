/**
 * 主机端测试用的最小 esp_err.h：只提供被测源码用到的类型与常量。
 * 本硬件编译始终使用 ESP-IDF 的同名头文件（本项目不改固件源码的包含关系）。
 */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NO_MEM 0x101

#define ESP_ERROR_CHECK(x) ((void)(x))

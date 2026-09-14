/**
 * esp_heap_caps 的主机替身：ns2_output 用它给 amiibo 镜像挑一块缓冲
 * （优先 PSRAM、退回内部 RAM）。主机上没有内存能力区分，两种能力都落到
 * 普通 malloc，被测逻辑一行不改。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL 0u
#define MALLOC_CAP_SPIRAM 0u
#define MALLOC_CAP_8BIT 0u

void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);

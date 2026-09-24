/**
 * esp_heap_caps 的主机替身：ns2_output 用它给 amiibo 镜像挑一块缓冲
 * （优先 PSRAM、退回内部 RAM）。主机上没有内存能力区分，两种能力都落到
 * 普通 malloc，被测逻辑一行不改；能力位给互异值，让按能力报余量的
 * 逻辑（ui_service 的堆文本）在两种能力下拿到不同读数。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u
#define MALLOC_CAP_DMA 8u

void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);

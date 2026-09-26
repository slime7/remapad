#include "esp_heap_caps.h"

#include <stdlib.h>

/** 内部 RAM 与 PSRAM 的固定读数：ui_service 的堆文本按这两个值格式化。 */
#define HOST_INTERNAL_TOTAL (400u * 1024u)
#define HOST_INTERNAL_FREE (200u * 1024u)
#define HOST_SPIRAM_TOTAL (8u * 1024u * 1024u)
#define HOST_SPIRAM_FREE (7u * 1024u * 1024u)

void *heap_caps_malloc(size_t size, uint32_t caps)
{
  (void)caps;
  return malloc(size);
}

void heap_caps_free(void *ptr)
{
  free(ptr);
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
  (void)alignment;
  (void)caps;
  return malloc(size);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
  return (caps & MALLOC_CAP_SPIRAM) != 0u ? HOST_SPIRAM_FREE : HOST_INTERNAL_FREE;
}

size_t heap_caps_get_total_size(uint32_t caps)
{
  return (caps & MALLOC_CAP_SPIRAM) != 0u ? HOST_SPIRAM_TOTAL : HOST_INTERNAL_TOTAL;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
  return heap_caps_get_free_size(caps) / 2u;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
  return heap_caps_get_free_size(caps) - (64u * 1024u);
}

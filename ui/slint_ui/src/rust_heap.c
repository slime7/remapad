/* Rust 侧 alloc crate 的内存出口：分配器本体与放置策略都在这里。
 * Rust 侧只保留 rustc 强制要求的 #[global_allocator] 转发声明（见 src/boundary.rs）。
 * 放置策略：大于 REMAPAD_SLINT_HEAP_PSRAM_THRESHOLD 的分配走 PSRAM（整帧缓冲 134 kB），
 * 其余走内部 RAM 且要求 DMA 可达（行带缓冲要交给 SPI DMA）。
 * 分配失败返回 NULL，由 Rust 侧的分配错误处理接管（默认 abort）。 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

/** 超过这个尺寸的分配走 PSRAM。 */
#define REMAPAD_SLINT_HEAP_PSRAM_THRESHOLD (64U * 1024U)
/** 堆默认保证的对齐。 */
#define REMAPAD_SLINT_HEAP_DEFAULT_ALIGN 4U

/* Rust 侧全局分配器的四个入口：由 boundary.rs 的转发声明调用。 */
void *remapad_slint_heap_alloc(size_t size, size_t align);
void *remapad_slint_heap_alloc_zeroed(size_t size, size_t align);
void remapad_slint_heap_dealloc(void *ptr, size_t size, size_t align);
void *remapad_slint_heap_realloc(void *ptr, size_t old_size, size_t align, size_t new_size);

/** 放置策略：大块进 PSRAM，其余进内部 RAM 并要求 DMA 可达。 */
static uint32_t heap_caps_for(size_t size)
{
  if (size >= REMAPAD_SLINT_HEAP_PSRAM_THRESHOLD) {
    return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  }
  return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
}

static void *heap_alloc(size_t size, size_t align)
{
  const uint32_t caps = heap_caps_for(size);
  if (align <= REMAPAD_SLINT_HEAP_DEFAULT_ALIGN) {
    return heap_caps_malloc(size, caps);
  }
  return heap_caps_aligned_alloc(align, size, caps);
}

void *remapad_slint_heap_alloc(size_t size, size_t align)
{
  return heap_alloc(size, align);
}

void *remapad_slint_heap_alloc_zeroed(size_t size, size_t align)
{
  void *ptr = heap_alloc(size, align);
  if (ptr != NULL) {
    memset(ptr, 0, size);
  }
  return ptr;
}

void remapad_slint_heap_dealloc(void *ptr, size_t size, size_t align)
{
  (void)size;
  (void)align;
  heap_caps_free(ptr);
}

void *remapad_slint_heap_realloc(void *ptr, size_t old_size, size_t align, size_t new_size)
{
  if (align <= REMAPAD_SLINT_HEAP_DEFAULT_ALIGN) {
    return heap_caps_realloc(ptr, new_size, heap_caps_for(new_size));
  }
  /* 对齐要求高于堆默认值时自己搬一次，保证新块仍满足对齐。 */
  void *new_ptr = heap_alloc(new_size, align);
  if (new_ptr != NULL) {
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    heap_caps_free(ptr);
  }
  return new_ptr;
}

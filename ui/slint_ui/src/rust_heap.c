/* Rust 侧 alloc crate 的内存出口：分配器本体与放置策略都在这里。
 * Rust 侧只保留 rustc 强制要求的 #[global_allocator] 转发声明（见 src/boundary.rs）。
 * 放置策略：全部走 PSRAM——内部 RAM 要留给 BLE/USB/WiFi 的任务栈与 DMA，
 * 而面板总线开了 psram_dma_direct，行带缓冲从 PSRAM 直读即可；PSRAM 分配按
 * 64 字节缓存行对齐，满足 esp_lcd 写回缓存行的对齐要求。
 * 分配失败返回 NULL，由 Rust 侧的分配错误处理接管（默认 abort）。 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

/** 堆默认保证的对齐。 */
#define REMAPAD_SLINT_HEAP_DEFAULT_ALIGN 4U
/** PSRAM 分配的对齐下限：esp_lcd 对 PSRAM 颜色缓冲做缓存行写回。 */
#define REMAPAD_SLINT_HEAP_PSRAM_ALIGN 64U

/* Rust 侧全局分配器的四个入口：由 boundary.rs 的转发声明调用。 */
void *remapad_slint_heap_alloc(size_t size, size_t align);
void *remapad_slint_heap_alloc_zeroed(size_t size, size_t align);
void remapad_slint_heap_dealloc(void *ptr, size_t size, size_t align);
void *remapad_slint_heap_realloc(void *ptr, size_t old_size, size_t align, size_t new_size);

static void *heap_alloc(size_t size, size_t align)
{
  size_t effective = align > REMAPAD_SLINT_HEAP_PSRAM_ALIGN ? align : REMAPAD_SLINT_HEAP_PSRAM_ALIGN;
  return heap_caps_aligned_alloc(effective, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
  /* 对齐要求高于堆默认值时自己搬一次，保证新块仍满足对齐。 */
  void *new_ptr = heap_alloc(new_size, align);
  if (new_ptr != NULL) {
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    heap_caps_free(ptr);
  }
  return new_ptr;
}

//! 与 C 世界的唯一边界：ESP-IDF 入口、固件回调与日志出口。
//! 本模块是全 crate 唯一允许出现 unsafe 的地方（CRATE 级 deny(unsafe_code)，其余模块编译期保证零 unsafe），
//! 每处 unsafe 都写明为什么无法避免：Rust 里调用 `extern "C"` 函数、调用 C 函数指针、
//! 以及解引用 C 侧传来的裸指针。分配器的内存出口与放置策略在 src/rust_heap.c，
//! 这里只保留 rustc 强制的 #[global_allocator] 转发项。
//!
//! 单线程前提：Slint 以 unsafe-single-threaded 模式运行，平台与界面状态只在 UI 任务上访问。

#![allow(unsafe_code)]
#![deny(unsafe_op_in_unsafe_fn)]

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_void, CStr};

use slint::SharedString;

use crate::abi::{self, ActionFn, Hooks, PollFn, Touch, UiState};
use slint::platform::software_renderer::Rgb565Pixel;

extern "C" {
  fn esp_timer_get_time() -> i64;
  fn esp_log_timestamp() -> u32;
  fn esp_log_write(level: u32, tag: *const c_char, format: *const c_char, ...);
  fn vTaskDelay(ticks: u32);
  #[link_name = "abort"]
  fn c_abort() -> !;
  fn remapad_slint_heap_alloc(size: usize, align: usize) -> *mut c_void;
  fn remapad_slint_heap_alloc_zeroed(size: usize, align: usize) -> *mut c_void;
  fn remapad_slint_heap_dealloc(ptr: *mut c_void, size: usize, align: usize);
  fn remapad_slint_heap_realloc(ptr: *mut c_void, old_size: usize, align: usize, new_size: usize) -> *mut c_void;
}

/// 全局分配器出口：四个入口原样转给 src/rust_heap.c，本身没有状态与放置策略。
///
/// 无法避免 unsafe：rustc 要求任何用到 `alloc` 的 crate 必须提供 `#[global_allocator]` 项，
/// 而 `GlobalAlloc` 的方法都是 `unsafe fn`，实现只能写在 Rust 源码里；
/// 堆的取用与放置策略（大块进 PSRAM、小块要求 DMA 可达）全部在 C 侧实现。
struct CHeap;

unsafe impl GlobalAlloc for CHeap {
  unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
    /* 无法避免 unsafe：调用 C 侧分配入口。 */
    unsafe { remapad_slint_heap_alloc(layout.size(), layout.align()) as *mut u8 }
  }

  unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
    /* 无法避免 unsafe：调用 C 侧释放入口。 */
    unsafe { remapad_slint_heap_dealloc(ptr as *mut c_void, layout.size(), layout.align()) }
  }

  unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
    /* 无法避免 unsafe：调用 C 侧清零分配入口。 */
    unsafe { remapad_slint_heap_alloc_zeroed(layout.size(), layout.align()) as *mut u8 }
  }

  unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
    /* 无法避免 unsafe：调用 C 侧重分配入口。 */
    let ptr = ptr as *mut c_void;
    unsafe { remapad_slint_heap_realloc(ptr, layout.size(), layout.align(), new_size) as *mut u8 }
  }
}

#[global_allocator]
static SLINT_HEAP: CHeap = CHeap;

/// 单调微秒时钟（esp_timer）。
pub fn now_us() -> i64 {
  unsafe { esp_timer_get_time() }
}

/// 上电以来的毫秒时间戳（日志行前缀用）。
pub fn log_timestamp() -> u32 {
  unsafe { esp_log_timestamp() }
}

/// 输出一行已经拼好的日志（整行经 "%s" 传入，消息里的 '%' 不会被当格式符）。
pub fn log_write(level: u32, tag: &CStr, line: &CStr) {
  unsafe { esp_log_write(level, tag.as_ptr(), c"%s".as_ptr(), line.as_ptr()) }
}

/// 让出 CPU 指定毫秒数。
pub fn delay_ms(ms: u32) {
  unsafe { vTaskDelay(ms_to_ticks(ms)) }
}

/// 致命错误退出（panic 处理器用）。
pub fn abort() -> ! {
  unsafe { c_abort() }
}

/// 读取 C 侧传入的 hooks 结构。
///
/// # Safety
/// `ptr` 必须指向一个已初始化且在本调用期间有效的 `remapad_slint_hooks_t`。
pub unsafe fn read_hooks(ptr: *const Hooks) -> Hooks {
  /* 无法避免的 unsafe：解引用 C 侧裸指针，有效性只能由调用方保证。 */
  unsafe { *ptr }
}

/// 调固件侧的状态快照回调（每 50 ms 一次）。
pub fn poll_state(poll: PollFn, state: &mut UiState, user: *mut c_void) {
  /* 无法避免的 unsafe：poll 是 C 侧函数指针；state 指向本函数栈上的有效值。 */
  unsafe { poll(state, user) }
}

/// 把固定名字的动作交回固件（名字来自 c"..." 字面量）。
pub fn dispatch_action_name(action: ActionFn, name: &CStr, value: i32, user: *mut c_void) {
  /* 无法避免的 unsafe：action 是 C 侧函数指针。 */
  unsafe { action(name.as_ptr(), value, user) }
}

/// 把 Slint 侧的动作交回固件（名字取自 `.slint` 的 action 回调）。
pub fn dispatch_action(action: ActionFn, name: &SharedString, value: i32, user: *mut c_void) {
  /* 无法避免的 unsafe：action 是 C 侧函数指针。SharedString 底层始终以 NUL 结尾
   * （Slint 对其 C++ 互操作有同样保证），因此可以直接当 C 字符串传出去。 */
  let text = name.as_str();
  unsafe { action(text.as_ptr() as *const c_char, value, user) }
}

/// 读固件侧传来的 C 字符串并立刻拷成界面属性文本（空指针或非法 UTF-8 用占位符）。
pub fn read_text(ptr: *const c_char, fallback: &str) -> SharedString {
  if ptr.is_null() {
    return SharedString::from(fallback);
  }
  /* 无法避免的 unsafe：解引用 C 侧裸指针，有效性只能由固件侧保证。 */
  match unsafe { CStr::from_ptr(ptr) }.to_str() {
    Ok(text) => SharedString::from(text),
    Err(_) => SharedString::from(fallback),
  }
}

/// 把一段行带交给面板驱动同步提交，几何按 width × height 声明（hooks 缺 transfer 时返回 ESP_ERR_INVALID_STATE）。
pub fn panel_transfer(hooks: &Hooks, band: &mut [Rgb565Pixel], x: i32, y: i32, width: i32, height: i32) -> i32 {
  let Some(transfer) = hooks.transfer else {
    return abi::ESP_ERR_INVALID_STATE;
  };
  /* 无法避免的 unsafe：transfer 是 C 侧函数指针；band 是有效切片，长度按 width × height 校验。 */
  if band.len() < (width.max(0) * height.max(0)) as usize {
    return abi::ESP_ERR_INVALID_ARG;
  }
  /* Rgb565Pixel 是 #[repr(transparent)] 的 u16 包装，面板驱动按 RGB565 小端处理同一块内存。 */
  unsafe { transfer(band.as_mut_ptr() as *mut u16, x, y, width, height) }
}

/// 采样当前触点（hooks 缺 touch_sample 时返回 0）。
pub fn touch_sample(hooks: &Hooks, out: &mut [Touch]) -> usize {
  let Some(sample) = hooks.touch_sample else {
    return 0;
  };
  /* 无法避免的 unsafe：sample 是 C 侧函数指针；out 是有效切片，容量按 len 传入。 */
  unsafe { sample(out.as_mut_ptr(), out.len()) }
}

/// 毫秒换算成 FreeRTOS 节拍（向上取整，至少 1 拍）。
pub fn ms_to_ticks(ms: u32) -> u32 {
  let ticks = (ms as u64 * crate::platform::FREERTOS_HZ as u64).div_ceil(1000);
  ticks.clamp(1, u32::MAX as u64 - 1) as u32
}

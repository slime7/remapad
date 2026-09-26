//! Slint 屏幕 UI 组件：界面由 ui/ 的 .slint 在构建期编译成 Rust 代码，平台层用
//! Slint 的 Rust 软件渲染器实现（见 platform.rs），对外只暴露 include/slint_ui.h 的 C 接口。
//! 状态与动作都在 UI 任务（事件循环所在任务）上下文交换，固件侧因此不需要加锁；
//! 数据面的高频报告不经过这里。
//!
//! unsafe 范围：crate 级 deny(unsafe_code)，只有 boundary.rs（ESP-IDF 入口、固件回调、
//! 全局分配器）与本文件里带 `#[allow(unsafe_code)]` 的 C ABI 入口被豁免，每处都写明
//! 无法避免的原因；平台层与宿主层完全不含 unsafe。

#![no_std]
#![deny(unsafe_code)]

extern crate alloc;

mod abi;
mod boundary;
mod host;
mod log;
mod platform;

slint::include_modules!();

use core::ffi::c_void;

/// 启动 UI：建平台与窗口、接好状态与动作回调；事件循环由 remapad_slint_ui_loop 占用调用任务。
///
/// # Safety
/// `hooks` 必须指向有效的 `remapad_slint_hooks_t`，且 transfer / touch_sample 都已接线。
/// 无法避免 unsafe：C ABI 用裸指针交接，Rust 侧无法在类型上表达其有效性。
#[allow(unsafe_code)]
#[no_mangle]
pub unsafe extern "C" fn remapad_slint_ui_start(
  hooks: *const abi::Hooks,
  poll: Option<abi::PollFn>,
  action: Option<abi::ActionFn>,
  user: *mut c_void,
) -> i32 {
  if hooks.is_null() {
    return abi::ESP_ERR_INVALID_ARG;
  }
  let hooks = unsafe { boundary::read_hooks(hooks) };
  host::start(hooks, poll, action, user)
}

/// 进入 Slint 事件循环：本函数不返回，独占调用它的任务。
#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn remapad_slint_ui_loop() {
  host::run();
}

/// 把整屏当前画面按 RGB565 小端拷进 out（240 × 280 × 2 字节），截图通路使用。
///
/// # Safety
/// `out` 必须指向至少 240 × 280 个 u16 的可写缓冲。
/// 无法避免 unsafe：C ABI 传裸指针，写入范围只能由调用方保证。
#[allow(unsafe_code)]
#[no_mangle]
pub unsafe extern "C" fn remapad_slint_ui_copy_frame(out: *mut u16) {
  let frame = platform::frame_ptr();
  if out.is_null() || frame.is_null() {
    return;
  }
  unsafe {
    core::ptr::copy_nonoverlapping(frame, out, platform::VIEW_WIDTH * platform::VIEW_HEIGHT);
  }
}

/// 当前帧缓冲（RGB565 小端，视口全宽）；平台未就绪时为 NULL。
#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn remapad_slint_ui_frame() -> *const u16 {
  platform::frame_ptr()
}

/// 读取并清零一个统计窗口的逐帧数据。
///
/// # Safety
/// `out` 必须指向一个有效的 `remapad_slint_stats_t`。
/// 无法避免 unsafe：C ABI 传裸指针，写入目标只能由调用方保证。
#[allow(unsafe_code)]
#[no_mangle]
pub unsafe extern "C" fn remapad_slint_ui_take_stats(out: *mut abi::Stats) {
  if out.is_null() {
    return;
  }
  unsafe { platform::take_stats(&mut *out) };
}

/// 息屏/亮屏开关：关闭时平台整段跳过触摸采样。
#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn remapad_slint_ui_set_touch_enabled(enabled: bool) {
  platform::set_touch_enabled(enabled);
}

/// 接下来 frames 帧逐帧打印渲染与提交耗时（串口 trace 命令；0 取默认长度）。
#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn remapad_slint_ui_trace_frames(frames: u32) {
  platform::trace_frames(frames);
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
  log::write(log::LEVEL_ERROR, c"slint_ui", format_args!("panic: {info}"));
  boundary::abort()
}

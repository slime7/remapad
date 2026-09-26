//! Slint 软件渲染平台：整帧 PSRAM 缓冲 + 按行带提交。
//! Slint 只重画 damage 区域，本平台把每条 damage 矩形按 48 行折成行带、拷进内部 RAM 的行带
//! 缓冲，再经面板驱动同步提交（驱动负责字节序转换与 DMA），因此刷新范围与渲染代价都只跟
//! 变化量走。缓冲用安全容器（分配器按尺寸放置），跨任务共享的只有原子量。

use core::cell::{Cell, RefCell};
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;

use slint::platform::software_renderer::{MinimalSoftwareWindow, PhysicalRegion, RepaintBufferType, Rgb565Pixel};
use slint::platform::{Platform, PlatformError, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{LogicalPosition, PhysicalSize};

use crate::abi::{self, Hooks, Stats, Touch};
use crate::boundary;
use crate::log::{log_info, log_warn};

/// 屏幕逻辑视口（与 .slint 根组件的宽高一致）。
pub const VIEW_WIDTH: usize = 240;
pub const VIEW_HEIGHT: usize = 280;

/// 行带高度：一条 240 × 48 × 2 = 23 kB。一条行带是一次同步 SPI 事务，事务本身的固定
/// 开销约 1 ms，行带取高一些能明显压低一次刷新的总耗时。
const BAND_ROWS: usize = 48;
/// 动画推进节拍：Slint 内部动画最多按这个间隔推进（与 60 Hz 的 UI 节拍一致）。
const ANIMATION_PACE_MS: u32 = 16;
/// 省电档（BLE 关闭）的动画推进节拍：与状态轮询同为 12 fps 等效。
const ANIMATION_PACE_POWER_SAVE_MS: u32 = 83;
/// 等待上限：定时器链为空时也周期性回到循环，保证触摸与重绘仍有执行机会。
const MAX_WAIT_MS: u32 = 100;
/// 省电档的等待上限：与省电档节拍一致，省电期间不再按 100 ms 唤醒。
const MAX_WAIT_POWER_SAVE_MS: u32 = 83;
const FRAME_PIXELS: usize = VIEW_WIDTH * VIEW_HEIGHT;
const BAND_PIXELS: usize = VIEW_WIDTH * BAND_ROWS;
/// 切分行带用的视口（与整帧缓冲、窗口尺寸同一份取值）。
const VIEWPORT: render_plan::Viewport = render_plan::Viewport {
  width: VIEW_WIDTH,
  height: VIEW_HEIGHT,
};

mod build_config {
  include!(concat!(env!("OUT_DIR"), "/remapad_slint_ui_config.rs"));
}

/// 目标配置的 FreeRTOS 节拍率（由构建脚本从 sdkconfig 的 CONFIG_FREERTOS_HZ 写入）。
pub const FREERTOS_HZ: u32 = build_config::FREERTOS_HZ;

/// 是否开发构建（由构建脚本按 REMAPAD_RELEASE 写入）：调试页只出现在开发构建的页表末位。
pub const UI_DEV: bool = build_config::UI_DEV;

/// 当前帧缓冲地址：截图通路在 UI 任务之外读，用原子量交接。
static FRAME_PTR: AtomicUsize = AtomicUsize::new(0);
/// 息屏期间整段跳过触摸采样（画面不可见，触点只会误触看不见的控件）。
static TOUCH_ENABLED: AtomicBool = AtomicBool::new(true);
/// 省电档（BLE 关闭）：事件循环按 12 fps 等效节拍唤醒与推进动画。
static POWER_SAVE: AtomicBool = AtomicBool::new(false);
/// trace 命令的剩余帧数：命令行任务可以随时置位。
static TRACE_FRAMES: AtomicU32 = AtomicU32::new(0);

/// 逐帧统计：窗口累计取走即清零，峰值与总帧数保留。
static STATS: StatsCounters = StatsCounters::new();

struct StatsCounters {
  frames: AtomicU32,
  window_frames: AtomicU32,
  window_render_us: AtomicU32,
  window_flush_us: AtomicU32,
  window_damage_px: AtomicU32,
  max_render_us: AtomicU32,
  max_flush_us: AtomicU32,
}

impl StatsCounters {
  const fn new() -> Self {
    Self {
      frames: AtomicU32::new(0),
      window_frames: AtomicU32::new(0),
      window_render_us: AtomicU32::new(0),
      window_flush_us: AtomicU32::new(0),
      window_damage_px: AtomicU32::new(0),
      max_render_us: AtomicU32::new(0),
      max_flush_us: AtomicU32::new(0),
    }
  }

  fn record(&self, render_us: u32, flush_us: u32, damage_px: u32) {
    self.frames.fetch_add(1, Ordering::Relaxed);
    self.window_frames.fetch_add(1, Ordering::Relaxed);
    self.window_render_us.fetch_add(render_us, Ordering::Relaxed);
    self.window_flush_us.fetch_add(flush_us, Ordering::Relaxed);
    self.window_damage_px.fetch_add(damage_px, Ordering::Relaxed);
    self.max_render_us.fetch_max(render_us, Ordering::Relaxed);
    self.max_flush_us.fetch_max(flush_us, Ordering::Relaxed);
  }

  /// 窗口累计是 5 秒量级，u32 足够（求和不会越过 43 亿微秒）。
  fn take(&self, out: &mut Stats) {
    out.frames = self.frames.load(Ordering::Relaxed);
    out.window_frames = self.window_frames.swap(0, Ordering::Relaxed);
    out.window_render_us = self.window_render_us.swap(0, Ordering::Relaxed) as u64;
    out.window_flush_us = self.window_flush_us.swap(0, Ordering::Relaxed) as u64;
    out.window_damage_px = self.window_damage_px.swap(0, Ordering::Relaxed) as u64;
    out.max_render_us = self.max_render_us.load(Ordering::Relaxed);
    out.max_flush_us = self.max_flush_us.load(Ordering::Relaxed);
  }
}

/// 平台：窗口、缓冲与触摸状态都挂在它上面，set_platform 之后由 Slint 持有。
pub struct EspPlatform {
  window: Rc<MinimalSoftwareWindow>,
  frame: RefCell<&'static mut [Rgb565Pixel]>,
  band: RefCell<&'static mut [Rgb565Pixel]>,
  hooks: Hooks,
  touch_down: Cell<bool>,
  touch_position: Cell<LogicalPosition>,
}

impl EspPlatform {
  /// 建平台：整帧缓冲（分配器按尺寸放进 PSRAM）、行带缓冲（内部 RAM 且 DMA 可达）与固定尺寸窗口。
  pub fn create(hooks: Hooks) -> Option<Self> {
    let frame = alloc_pixels(FRAME_PIXELS)?;
    let band = alloc_pixels(BAND_PIXELS)?;
    FRAME_PTR.store(frame.as_ptr() as usize, Ordering::Release);

    let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
    window.set_size(PhysicalSize::new(VIEW_WIDTH as u32, VIEW_HEIGHT as u32));
    log_info!(
      c"slint_platform",
      "platform ready: frame {} B, band {} B",
      FRAME_PIXELS * 2,
      BAND_PIXELS * 2
    );
    Some(Self {
      window,
      frame: RefCell::new(frame),
      band: RefCell::new(band),
      hooks,
      touch_down: Cell::new(false),
      touch_position: Cell::new(LogicalPosition::new(0.0, 0.0)),
    })
  }

  /// 采样一次触点并把它交给 Slint（按下/抬起成对，抬起同时退出悬停）。
  fn poll_touch(&self) {
    let mut contacts = [Touch::default(); 1];
    let count = boundary::touch_sample(&self.hooks, &mut contacts);
    let position = LogicalPosition::new(contacts[0].x as f32, contacts[0].y as f32);
    if count > 0 {
      self.window.dispatch_event(WindowEvent::PointerMoved { position });
      if !self.touch_down.get() {
        self.window.dispatch_event(WindowEvent::PointerPressed {
          position,
          button: PointerEventButton::Left,
        });
        self.touch_down.set(true);
      }
      self.touch_position.set(position);
    } else if self.touch_down.get() {
      self.window.dispatch_event(WindowEvent::PointerReleased {
        position: self.touch_position.get(),
        button: PointerEventButton::Left,
      });
      self.window.dispatch_event(WindowEvent::PointerExited);
      self.touch_down.set(false);
    }
  }

  /// 把一条 damage 矩形折成行带逐条提交（切分与拷贝在 render-plan，见 ui/render-plan）。
  fn flush_region(&self, region: &PhysicalRegion) {
    let mut band = self.band.borrow_mut();
    let frame = self.frame.borrow();
    for (origin, size) in region.iter() {
      let damage = render_plan::Damage {
        x: origin.x,
        y: origin.y,
        width: size.width,
        height: size.height,
      };
      for line in render_plan::bands(damage, VIEWPORT, BAND_ROWS) {
        let pixels = render_plan::copy_band(&frame[..], &mut band[..], VIEW_WIDTH, line);
        let result = boundary::panel_transfer(
          &self.hooks,
          &mut band[..pixels],
          line.x as i32,
          line.y as i32,
          line.width as i32,
          line.rows as i32,
        );
        if result != abi::ESP_OK {
          log_warn!(c"slint_platform", "band transfer failed: {}", result);
        }
      }
    }
  }

  /// 记录一帧的渲染与提交耗时（trace 命令在场时逐帧打印）。
  fn record(&self, region: &PhysicalRegion, render_us: u32, flush_us: u32) {
    let mut damage_px: u32 = 0;
    let mut rects: u32 = 0;
    for (_, size) in region.iter() {
      damage_px += size.width * size.height;
      rects += 1;
    }
    STATS.record(render_us, flush_us, damage_px);

    let remaining = TRACE_FRAMES.load(Ordering::Relaxed);
    if remaining > 0 {
      TRACE_FRAMES.store(remaining - 1, Ordering::Relaxed);
      log_info!(
        c"slint_platform",
        "trace frame: render_us={} flush_us={} damage_px={} rects={}",
        render_us,
        flush_us,
        damage_px,
        rects
      );
    }
  }

  /// 事件循环：推进动画与定时器、采样触摸、按需重绘并提交，然后按最近的唤醒时刻让出 CPU。
  fn run_forever(&self) -> ! {
    loop {
      slint::platform::update_timers_and_animations();

      if TOUCH_ENABLED.load(Ordering::Relaxed) {
        self.poll_touch();
      }

      self.window.draw_if_needed(|renderer| {
        let started = boundary::now_us();
        let region = renderer.render(&mut self.frame.borrow_mut()[..], VIEW_WIDTH);
        let rendered = boundary::now_us();
        self.flush_region(&region);
        let flushed = boundary::now_us();
        self.record(
          &region,
          (rendered - started).max(0) as u32,
          (flushed - rendered).max(0) as u32,
        );
      });

      /* 动画期间按固定节拍推进：不设上限的话动画状态会把事件循环拉成自旋，
       * 每帧还要走一遍整棵控件树的渲染，实测能饿死 IDLE 任务并触发看门狗。
       * 省电档（BLE 关闭）把两档间隔一起降到 12 fps 等效。 */
      let power_save = POWER_SAVE.load(Ordering::Relaxed);
      let mut wait_ms = if power_save {
        MAX_WAIT_POWER_SAVE_MS
      } else {
        MAX_WAIT_MS
      };
      if let Some(until) = slint::platform::duration_until_next_timer_update() {
        wait_ms = wait_ms.min(until.as_millis() as u32);
      }
      if self.window.has_active_animations() {
        let pace = if power_save {
          ANIMATION_PACE_POWER_SAVE_MS
        } else {
          ANIMATION_PACE_MS
        };
        wait_ms = wait_ms.min(pace);
      }
      boundary::delay_ms(wait_ms);
    }
  }
}

impl Platform for EspPlatform {
  fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
    let adapter: Rc<dyn WindowAdapter> = self.window.clone();
    Ok(adapter)
  }

  fn duration_since_start(&self) -> core::time::Duration {
    core::time::Duration::from_micros(boundary::now_us().max(0) as u64)
  }

  fn run_event_loop(&self) -> Result<(), PlatformError> {
    self.run_forever()
  }
}

/// 当前帧缓冲（RGB565 小端，视口全宽）；平台未就绪时为 NULL。
pub fn frame_ptr() -> *const u16 {
  let address = FRAME_PTR.load(Ordering::Acquire);
  if address == 0 {
    core::ptr::null()
  } else {
    address as *const u16
  }
}

/// 分配一块清零的像素缓冲；内存不足返回 None（不 panic，交给调用方报 ESP_ERR_NO_MEM）。
fn alloc_pixels(count: usize) -> Option<&'static mut [Rgb565Pixel]> {
  let mut pixels: Vec<Rgb565Pixel> = Vec::new();
  pixels.try_reserve_exact(count).ok()?;
  pixels.resize(count, Rgb565Pixel(0));
  Some(Box::leak(pixels.into_boxed_slice()))
}

/// 息屏/亮屏开关：关闭时平台整段跳过触摸采样。
pub fn set_touch_enabled(enabled: bool) {
  TOUCH_ENABLED.store(enabled, Ordering::Relaxed);
}

/// 省电档开关：BLE 关闭时事件循环按 12 fps 等效节拍唤醒。
pub fn set_power_save(enabled: bool) {
  POWER_SAVE.store(enabled, Ordering::Relaxed);
}

/// 读取并清零一个统计窗口的逐帧数据。
pub fn take_stats(out: &mut Stats) {
  STATS.take(out);
}

/// 接下来 frames 帧逐帧打印渲染与提交耗时。
pub fn trace_frames(frames: u32) {
  TRACE_FRAMES.store(frames, Ordering::Relaxed);
}

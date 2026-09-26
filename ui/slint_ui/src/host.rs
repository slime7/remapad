//! 界面宿主：把固件状态写进界面属性、把界面动作交回固件，并驱动手柄按键导航。
//! 状态与动作都在 UI 任务（事件循环所在任务）上下文交换；翻页、焦点与提示相位都由本模块持有，
//! 触摸与手柄按键走同一条路径，固件只负责数据面的状态与命令。
//! 界面文案全部来自 ui/ 的 .slint（固件回发的文本不会被烘焙）；本模块与平台层都不含 unsafe。

use core::cell::Cell;
use core::ffi::c_void;
use core::time::Duration;

use alloc::boxed::Box;

use slint::{ComponentHandle, SharedString, Timer, TimerMode};

use crate::abi::{self, ActionFn, Hooks, PollFn, Stats, UiState};
use crate::boundary;
use crate::log::{log_error, log_info};
use crate::platform;
use crate::App;

/// 状态轮询间隔：呼吸与旋转提示按这个节拍推进。
const POLL_MS: u64 = 50;
/// 省电档（BLE 关闭）的状态轮询间隔：与平台节拍同为 12 fps 等效。
const POLL_POWER_SAVE_MS: u64 = 83;
/// 手柄按键后焦点环的可见窗口（微秒）。
const PAD_IDLE_US: i64 = 4 * 1000 * 1000;
/// 周期统计窗口：5 秒一行。
const STATS_WINDOW_US: i64 = 5 * 1000 * 1000;
/// 主机图标呼吸周期：50 ms 一步，24 步为 1.2 秒。
const BREATH_STEPS: i32 = 24;
/// 广播提示的旋转周期：50 ms 一步，8 步约 0.9 秒一圈。
const SPIN_STEPS: i32 = 8;
const SPIN_STEP_TICKS: i32 = 2;
/// 页面数量：ui/src/app.slint 的页表；开发构建末位追加调试页（未指定 release 即开发构建）。
const PAGE_COUNT: i32 = if crate::platform::UI_DEV { 8 } else { 7 };
/// 事件循环意外退出后的兜底休眠（毫秒）。
const IDLE_FALLBACK_MS: u32 = 1000;

/* 手柄按键位：与 dp/dp_ui.h 的 DP_UI_BTN_* 同值（UI 按键位规范）。 */
const BTN_UP: i32 = 0x0010;
const BTN_RIGHT: i32 = 0x0020;
const BTN_DOWN: i32 = 0x0040;
const BTN_LEFT: i32 = 0x0080;
const BTN_L1: i32 = 0x0100;
const BTN_R1: i32 = 0x0200;
const BTN_CIRCLE: i32 = 0x2000;
const BTN_CROSS: i32 = 0x4000;

/// 宿主状态：都在 UI 任务上读写，标量用 Cell 以便从 Slint 回调里原地改。
/// 宿主与组件句柄都按进程生命周期泄漏（界面一直活着，不必回收）。
struct Glue {
  poll: Option<PollFn>,
  action: Option<ActionFn>,
  user: *mut c_void,
  timer: Timer,
  poll_ms: Cell<u64>,
  last: Cell<UiState>,
  page: Cell<i32>,
  focus: Cell<i32>,
  dialog_focus: Cell<i32>,
  last_key_us: Cell<i64>,
  spin_step: Cell<i32>,
  spin_ticks: Cell<i32>,
  breath_step: Cell<i32>,
  stats_due_us: Cell<i64>,
}

/// 启动 UI：建平台与窗口、接好状态与动作回调，并起 50 ms 轮询定时器。
pub fn start(hooks: Hooks, poll: Option<PollFn>, action: Option<ActionFn>, user: *mut c_void) -> i32 {
  let Some(platform) = platform::EspPlatform::create(hooks) else {
    return abi::ESP_ERR_NO_MEM;
  };
  if slint::platform::set_platform(Box::new(platform)).is_err() {
    return abi::ESP_ERR_INVALID_STATE;
  }

  let component = match App::new() {
    Ok(component) => component,
    Err(_) => return abi::ESP_ERR_INVALID_STATE,
  };
  let ui: &'static App = Box::leak(Box::new(component));
  let glue: &'static Glue = Box::leak(Box::new(Glue {
    poll,
    action,
    user,
    timer: Timer::default(),
    poll_ms: Cell::new(POLL_MS),
    last: Cell::new(UiState::default()),
    page: Cell::new(0),
    focus: Cell::new(-1),
    dialog_focus: Cell::new(0),
    last_key_us: Cell::new(0),
    spin_step: Cell::new(0),
    spin_ticks: Cell::new(0),
    breath_step: Cell::new(0),
    stats_due_us: Cell::new(0),
  }));

  ui.on_action(move |name, value| on_action(glue, ui, &name, value));
  ui.set_page(0);
  ui.set_page_count(PAGE_COUNT);

  /* 首帧之前先取一次状态，避免开机闪一帧默认值。 */
  on_timer(glue, ui);
  glue.stats_due_us.set(boundary::now_us() + STATS_WINDOW_US);
  glue
    .timer
    .start(TimerMode::Repeated, Duration::from_millis(POLL_MS), move || {
      on_timer(glue, ui)
    });

  if ui.show().is_err() {
    return abi::ESP_ERR_INVALID_STATE;
  }
  log_info!(c"slint_ui", "UI ready: {} pages, poll {} ms", PAGE_COUNT, POLL_MS);
  abi::ESP_OK
}

/// 进入 Slint 事件循环：本函数不返回，独占调用它的任务。
pub fn run() -> ! {
  if slint::run_event_loop().is_err() {
    log_error!(c"slint_ui", "event loop unavailable");
  }
  loop {
    boundary::delay_ms(IDLE_FALLBACK_MS);
  }
}

/// 一次状态轮询：取快照、处理手柄按键、把状态写进界面，并按窗口打印画面统计。
fn on_timer(glue: &Glue, ui: &App) {
  let now_us = boundary::now_us();
  let mut state = UiState::default();
  if let Some(poll) = glue.poll {
    boundary::poll_state(poll, &mut state, glue.user);
  }

  if state.dialog != 0 && glue.last.get().dialog == 0 {
    glue.dialog_focus.set(0);
    ui.set_dialog_focus(0);
  }
  handle_pad(glue, ui, &state, now_us);
  apply_state(glue, ui, &state);

  /* 焦点环只在手柄操控期间可见：固件模式或最近 4 秒内有按键。
   * 界面侧按这个窗口决定是否画环，触摸操作因此不会被焦点框跟着走。 */
  let pad_active = pad_focus_visible(glue, now_us);
  ui.set_pad_active(pad_active);
  if !pad_active && glue.focus.get() >= 0 {
    set_focus(glue, ui, -1);
  }
  glue.last.set(state);

  if now_us >= glue.stats_due_us.get() {
    let mut stats = Stats::default();
    platform::take_stats(&mut stats);
    let frames = stats.window_frames.max(1) as u64;
    log_info!(
      c"slint_ui",
      "frames={} avg_render_us={} avg_flush_us={} avg_damage_px={} max_render_us={} max_flush_us={}",
      stats.frames,
      (stats.window_render_us / frames) as u32,
      (stats.window_flush_us / frames) as u32,
      (stats.window_damage_px / frames) as u32,
      stats.max_render_us,
      stats.max_flush_us
    );
    glue.stats_due_us.set(now_us + STATS_WINDOW_US);
  }
}

/// 把一轮快照写进界面属性（数值、文本与呼吸/旋转相位都在这里对齐）。
fn apply_state(glue: &Glue, ui: &App, state: &UiState) {
  ui.set_backlight(state.backlight);
  ui.set_battery_percent(state.battery_percent);
  ui.set_pairing(state.pairing);
  ui.set_notice(state.notice);
  ui.set_usb_role(state.usb_role);
  ui.set_pc_link(state.pc_link);
  ui.set_pad_attached(state.pad_attached);
  ui.set_pad_family(state.pad_family);
  ui.set_player_led(state.player_led);
  ui.set_pad_ui_mode(state.pad_ui_mode);
  ui.set_ota_phase(state.ota_phase);
  ui.set_ota_percent(state.ota_percent);
  ui.set_selected_colorway(state.selected_colorway);
  ui.set_controller_address(boundary::read_text(state.controller_address, "--"));
  ui.set_ds_touchpad_plus_minus(state.ds_touchpad_plus_minus);
  ui.set_ds_capture_key(state.ds_capture_key);
  ui.set_firmware_version(boundary::read_text(state.firmware_version, "-"));
  ui.set_heap_text(boundary::read_text(state.heap_text, "-"));
  ui.set_psram_text(boundary::read_text(state.psram_text, "-"));
  ui.set_battery_text(boundary::read_text(state.battery_text, "-"));
  ui.set_debug_flash(state.debug_flash);
  ui.set_dialog(state.dialog);
  ui.set_powering_off(state.powering_off);
  ui.set_rebooting(state.rebooting);
  platform::set_touch_enabled(state.screen_on);
  /* 省电档（BLE 关闭）：轮询与动画节拍一起降到 12 fps 等效。 */
  platform::set_power_save(state.power_save);
  set_poll_interval(glue, state.power_save);

  /* 主机图标呼吸：广播与扫描期间按 1.2 秒周期起伏，其余时间常亮。 */
  let searching = state.pairing == 2 || state.pairing == 3;
  if searching {
    let step = (glue.breath_step.get() + 1) % BREATH_STEPS;
    glue.breath_step.set(step);
    let wave = 0.5 - 0.5 * libm::cos(2.0 * core::f64::consts::PI * step as f64 / BREATH_STEPS as f64);
    ui.set_breath((0.25 + wave * 0.75) as f32);
  } else if ui.get_breath() != 1.0 {
    glue.breath_step.set(0);
    ui.set_breath(1.0);
  }

  /* 广播提示的旋转相位：只在扫描/连接/配对中推进，停下即归零。 */
  let spinning = searching || state.pairing == 4 || state.pairing == 5;
  if spinning {
    let ticks = (glue.spin_ticks.get() + 1) % SPIN_STEP_TICKS;
    glue.spin_ticks.set(ticks);
    if ticks == 0 {
      glue.spin_step.set((glue.spin_step.get() + 1) % SPIN_STEPS);
    }
  } else {
    glue.spin_step.set(0);
  }
  ui.set_spinner_phase(glue.spin_step.get());
}

/// 省电档切换轮询节拍：改周期不重建回调，切档不丢当前状态。
fn set_poll_interval(glue: &Glue, power_save: bool) {
  let period = if power_save { POLL_POWER_SAVE_MS } else { POLL_MS };
  if glue.poll_ms.get() == period {
    return;
  }
  glue.poll_ms.set(period);
  glue.timer.set_interval(Duration::from_millis(period));
}

/// 手柄按键导航：边沿触发，左右（含 L1/R1）翻页、上下移焦点、圆圈确认、叉键取消弹窗。
fn handle_pad(glue: &Glue, ui: &App, state: &UiState, now_us: i64) {
  let pressed = state.buttons & !glue.last.get().buttons;
  if pressed != 0 {
    glue.last_key_us.set(now_us);
  }
  if pressed & (BTN_RIGHT | BTN_R1) != 0 {
    step_page(glue, ui, 1);
  } else if pressed & (BTN_LEFT | BTN_L1) != 0 {
    step_page(glue, ui, -1);
  }
  if pressed & BTN_DOWN != 0 {
    step_focus(glue, ui, 1);
  }
  if pressed & BTN_UP != 0 {
    step_focus(glue, ui, -1);
  }
  if pressed & BTN_CROSS != 0 && state.dialog != 0 {
    dispatch(glue, c"dialog-cancel", 0);
  } else if pressed & BTN_CIRCLE != 0 {
    ui.invoke_activate_focused();
  }
}

/// .slint 侧的动作入口：翻页与弹窗焦点在本地就地处理，其余原样交回固件。
fn on_action(glue: &Glue, ui: &App, name: &SharedString, value: i32) {
  match name.as_str() {
    "next-page" => {
      step_page(glue, ui, 1);
      return;
    }
    "prev-page" => {
      step_page(glue, ui, -1);
      return;
    }
    "dialog-focus" => {
      glue.dialog_focus.set(value);
      ui.set_dialog_focus(value);
      return;
    }
    _ => {}
  }
  if let Some(action) = glue.action {
    boundary::dispatch_action(action, name, value, glue.user);
  }
}

/// 交一个固定名字的动作给固件侧回调。
fn dispatch(glue: &Glue, name: &core::ffi::CStr, value: i32) {
  if let Some(action) = glue.action {
    boundary::dispatch_action_name(action, name, value, glue.user);
  }
}

fn pad_focus_visible(glue: &Glue, now_us: i64) -> bool {
  if glue.last.get().pad_ui_mode {
    return true;
  }
  let last_key = glue.last_key_us.get();
  last_key != 0 && now_us - last_key <= PAD_IDLE_US
}

fn set_focus(glue: &Glue, ui: &App, index: i32) {
  glue.focus.set(index);
  ui.set_focus_index(index);
}

fn apply_page(glue: &Glue, ui: &App) {
  let count = ui.get_focus_count();
  ui.set_page(glue.page.get());
  set_focus(glue, ui, if count > 0 { 0 } else { -1 });
  dispatch(glue, c"page", glue.page.get());
}

fn step_page(glue: &Glue, ui: &App, delta: i32) {
  let mut next = glue.page.get() + delta;
  if next < 0 {
    next = PAGE_COUNT - 1;
  } else if next >= PAGE_COUNT {
    next = 0;
  }
  glue.page.set(next);
  apply_page(glue, ui);
}

fn step_focus(glue: &Glue, ui: &App, delta: i32) {
  let count = ui.get_focus_count();
  if count <= 0 {
    set_focus(glue, ui, -1);
    return;
  }
  let index = glue.focus.get();
  let next = if index < 0 {
    if delta > 0 {
      0
    } else {
      count - 1
    }
  } else {
    (index + delta + count) % count
  };
  set_focus(glue, ui, next);
}

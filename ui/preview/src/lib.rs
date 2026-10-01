//! 屏幕界面的 WASM 预览入口：提供预览实例生命周期管理、动作注入与状态读写接口。

use std::cell::RefCell;

use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

slint::include_modules!();

thread_local! {
    /// 当前预览实例：页面加载后由 remapad_start 建立一次，之后所有接口都从这里取。
    static APP: RefCell<Option<WasmApp>> = const { RefCell::new(None) };
}

#[wasm_bindgen]
extern "C" {
  /// 页面脚本注入的回调：把就绪标记写回 window（e2e 用例等它变真再开始断言）。
  #[wasm_bindgen(js_namespace = globalThis)]
  fn setRemapadReady(ready: bool);
  /// 把消息写进浏览器控制台：wasm 侧没有日志后端，事件循环退出只能落在这里。
  #[wasm_bindgen(js_namespace = console)]
  fn error(message: &str);
}

/// 在已启动的预览实例上跑一步；未启动时返回 JS 可见的错误。
fn with_app<T>(run: impl FnOnce(&WasmApp) -> T) -> Result<T, JsValue> {
  APP.with(|slot| {
    slot
      .borrow()
      .as_ref()
      .map(run)
      .ok_or_else(|| JsValue::from_str("预览未启动：先调用 remapad_start()"))
  })
}

/// 启动预览：装 winit 后端（web 上走 spawn 通路）、建窗口、存进槽位并进入事件循环；重复调用报错。
#[wasm_bindgen]
pub fn remapad_start() -> Result<(), JsValue> {
  console_error_panic_hook::set_once();
  let already = APP.with(|slot| slot.borrow().is_some());
  if already {
    return Err(JsValue::from_str("预览已启动"));
  }
  let backend = i_slint_backend_winit::Backend::builder()
    .with_spawn_event_loop(true)
    .build()
    .map_err(|e| JsValue::from_str(&format!("创建 winit 后端失败: {e}")))?;
  slint::platform::set_platform(Box::new(backend)).map_err(|e| JsValue::from_str(&format!("装配平台失败: {e}")))?;
  let app = WasmApp::new().map_err(|e| JsValue::from_str(&format!("创建预览窗口失败: {e}")))?;
  app
    .show()
    .map_err(|e| JsValue::from_str(&format!("显示预览窗口失败: {e}")))?;
  APP.with(|slot| *slot.borrow_mut() = Some(app));
  // wasm 上 run_event_loop 走 winit 的 spawn：立即返回，后续由浏览器帧回调驱动。
  if let Err(e) = slint::run_event_loop() {
    error(&format!("事件循环退出: {e}"));
  }
  setRemapadReady(true);
  Ok(())
}

/// 注入一条界面动作（与固件的动作同名同参），HTML 控制台与设备画面点按走同一条路。
#[wasm_bindgen]
pub fn remapad_action(name: String, value: i32) -> Result<(), JsValue> {
  with_app(|app| app.invoke_action(name.into(), value))
}

/// 焦点步进（实机的上下键）：到底回绕到另一端，语义在 preview-core.slint 里。
#[wasm_bindgen]
pub fn remapad_focus_step(delta: i32) -> Result<(), JsValue> {
  with_app(|app| app.invoke_focus_step(delta))
}

/// 确认键：走设备画面自己的按页焦点分发。
#[wasm_bindgen]
pub fn remapad_activate_focused() -> Result<(), JsValue> {
  with_app(|app| app.invoke_activate_focused())
}

/// 模拟状态补丁：只写出现的字段，其余保持原值；JS 侧传 camelCase 对象。
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct StatePatch {
  page: Option<i32>,
  page_count: Option<i32>,
  focus_index: Option<i32>,
  pad_active: Option<bool>,
  backlight: Option<i32>,
  battery_percent: Option<i32>,
  pairing: Option<i32>,
  notice: Option<i32>,
  tick: Option<i32>,
  usb_role: Option<i32>,
  pc_link: Option<bool>,
  pad_attached: Option<bool>,
  pad_family: Option<i32>,
  player_led: Option<i32>,
  ota_percent: Option<i32>,
  selected_colorway: Option<i32>,
  ds_touchpad_plus_minus: Option<bool>,
  ds_capture_key: Option<bool>,
  debug_flash: Option<i32>,
  dialog: Option<i32>,
  dialog_focus: Option<i32>,
  rebooting: Option<bool>,
  powering_off: Option<bool>,
  netlog_state: Option<i32>,
  netlog_addr: Option<String>,
  netlog_rssi: Option<i32>,
  log: Option<String>,
}

/// 预览的全部模拟状态：e2e 用例在浏览器侧断言语义事实的唯一入口。
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Snapshot {
  page: i32,
  page_count: i32,
  focus_index: i32,
  focus_count: i32,
  pad_active: bool,
  backlight: i32,
  battery_percent: i32,
  pairing: i32,
  notice: i32,
  tick: i32,
  usb_role: i32,
  pc_link: bool,
  pad_attached: bool,
  pad_family: i32,
  player_led: i32,
  ota_percent: i32,
  selected_colorway: i32,
  ds_touchpad_plus_minus: bool,
  ds_capture_key: bool,
  debug_flash: i32,
  dialog: i32,
  dialog_focus: i32,
  rebooting: bool,
  powering_off: bool,
  netlog_state: i32,
  netlog_addr: String,
  netlog_rssi: i32,
  log: String,
}

/// 读取整幅快照。
#[wasm_bindgen]
pub fn remapad_snapshot() -> Result<JsValue, JsValue> {
  with_app(|app| Snapshot {
    page: app.get_page(),
    page_count: app.get_page_count(),
    focus_index: app.get_focus_index(),
    focus_count: app.get_focus_count(),
    pad_active: app.get_pad_active(),
    backlight: app.get_backlight(),
    battery_percent: app.get_battery_percent(),
    pairing: app.get_pairing(),
    notice: app.get_notice(),
    tick: app.get_tick(),
    usb_role: app.get_usb_role(),
    pc_link: app.get_pc_link(),
    pad_attached: app.get_pad_attached(),
    pad_family: app.get_pad_family(),
    player_led: app.get_player_led(),
    ota_percent: app.get_ota_percent(),
    selected_colorway: app.get_selected_colorway(),
    ds_touchpad_plus_minus: app.get_ds_touchpad_plus_minus(),
    ds_capture_key: app.get_ds_capture_key(),
    debug_flash: app.get_debug_flash(),
    dialog: app.get_dialog(),
    dialog_focus: app.get_dialog_focus(),
    rebooting: app.get_rebooting(),
    powering_off: app.get_powering_off(),
    netlog_state: app.get_netlog_state(),
    netlog_addr: app.get_netlog_addr().to_string(),
    netlog_rssi: app.get_netlog_rssi(),
    log: app.get_log().to_string(),
  })
  .map(|snapshot| serde_wasm_bindgen::to_value(&snapshot).expect("快照序列化不会失败"))
}

/// 写入模拟状态补丁；只替换出现的字段。
#[wasm_bindgen]
pub fn remapad_set_state(patch: JsValue) -> Result<(), JsValue> {
  let patch: StatePatch =
    serde_wasm_bindgen::from_value(patch).map_err(|e| JsValue::from_str(&format!("状态补丁解析失败: {e}")))?;
  with_app(|app| {
    if let Some(v) = patch.page {
      app.set_page(v);
    }
    if let Some(v) = patch.page_count {
      app.set_page_count(v);
    }
    if let Some(v) = patch.focus_index {
      app.set_focus_index(v);
    }
    if let Some(v) = patch.pad_active {
      app.set_pad_active(v);
    }
    if let Some(v) = patch.backlight {
      app.set_backlight(v);
    }
    if let Some(v) = patch.battery_percent {
      app.set_battery_percent(v);
    }
    if let Some(v) = patch.pairing {
      app.set_pairing(v);
    }
    if let Some(v) = patch.notice {
      app.set_notice(v);
    }
    if let Some(v) = patch.tick {
      app.set_tick(v);
    }
    if let Some(v) = patch.usb_role {
      app.set_usb_role(v);
    }
    if let Some(v) = patch.pc_link {
      app.set_pc_link(v);
    }
    if let Some(v) = patch.pad_attached {
      app.set_pad_attached(v);
    }
    if let Some(v) = patch.pad_family {
      app.set_pad_family(v);
    }
    if let Some(v) = patch.player_led {
      app.set_player_led(v);
    }
    if let Some(v) = patch.ota_percent {
      app.set_ota_percent(v);
    }
    if let Some(v) = patch.selected_colorway {
      app.set_selected_colorway(v);
    }
    if let Some(v) = patch.ds_touchpad_plus_minus {
      app.set_ds_touchpad_plus_minus(v);
    }
    if let Some(v) = patch.ds_capture_key {
      app.set_ds_capture_key(v);
    }
    if let Some(v) = patch.debug_flash {
      app.set_debug_flash(v);
    }
    if let Some(v) = patch.dialog {
      app.set_dialog(v);
    }
    if let Some(v) = patch.dialog_focus {
      app.set_dialog_focus(v);
    }
    if let Some(v) = patch.rebooting {
      app.set_rebooting(v);
    }
    if let Some(v) = patch.powering_off {
      app.set_powering_off(v);
    }
    if let Some(v) = patch.netlog_state {
      app.set_netlog_state(v);
    }
    if let Some(v) = patch.netlog_addr {
      app.set_netlog_addr(v.into());
    }
    if let Some(v) = patch.netlog_rssi {
      app.set_netlog_rssi(v);
    }
    if let Some(v) = patch.log {
      app.set_log(v.into());
    }
  })
}

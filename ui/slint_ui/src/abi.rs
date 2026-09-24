//! C ABI 类型：与 include/slint_ui.h 一一对应。
//! 布局断言按 xtensa（32 位）手算的偏移核对，改头文件里的字段顺序会在这里编译失败。

use core::ffi::{c_char, c_void};

pub const ESP_OK: i32 = 0;
pub const ESP_ERR_NO_MEM: i32 = 0x0101;
pub const ESP_ERR_INVALID_ARG: i32 = 0x0102;
pub const ESP_ERR_INVALID_STATE: i32 = 0x0103;

/// 一次采样得到的触点，坐标为逻辑视口像素。
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Touch {
    pub x: u16,
    pub y: u16,
}

/// 状态快照回调（UI 任务上下文，每 50 ms 一次）。
pub type PollFn = unsafe extern "C" fn(state: *mut UiState, user: *mut c_void);

/// 动作回调（UI 任务上下文）：name 是 .slint 侧的动作名，value 是参数。
pub type ActionFn = unsafe extern "C" fn(name: *const c_char, value: i32, user: *mut c_void);

/// 把一段 RGB565 小端像素写到面板窗口，阻塞到传输完成。
pub type TransferFn =
    unsafe extern "C" fn(pixels: *mut u16, x: i32, y: i32, width: i32, height: i32) -> i32;

/// 采样当前触点，返回有效触点数。
pub type TouchSampleFn = unsafe extern "C" fn(out: *mut Touch, capacity: usize) -> usize;

/// 平台需要的硬件入口：面板提交与触摸采样都由固件侧提供。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Hooks {
    pub transfer: Option<TransferFn>,
    pub touch_sample: Option<TouchSampleFn>,
}

/// 界面状态快照：一次轮询填满，UI 侧按字段写入对应属性。
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiState {
    /// 背光百分比（0-100）。
    pub backlight: i32,
    /// 电池百分比与充电标记。
    pub battery_percent: i32,
    /// 配对状态：0 idle、1 paired、2 advertising、3 scanning、4 pairing、5 connected。
    pub pairing: i32,
    /// 命令应答提示（0 无）。
    pub notice: i32,
    /// USB 角色：0 device（串口）、1 host（手柄）。
    pub usb_role: i32,
    /// PC 是否连在串口上。
    pub pc_link: bool,
    /// 物理手柄是否接入。
    pub pad_attached: bool,
    /// 手柄家族（0 PAD、1 PS、2 XBOX、3 NS、4 STEAM）。
    pub pad_family: i32,
    /// 主机下发的玩家序号灯掩码。
    pub player_led: i32,
    /// 手柄操控屏幕模式是否生效。
    pub pad_ui_mode: bool,
    /// UI 按键位（见 dp/dp_ui.h 的 DP_UI_BTN_*），未按下为 0。
    pub buttons: i32,
    /// OTA 阶段与百分比。
    pub ota_phase: i32,
    pub ota_percent: i32,
    /// 手柄配色命中款（0-3），未命中为 -1。
    pub selected_colorway: i32,
    /// 对外蓝牙地址（显示序），未同步时传 "--"。
    pub controller_address: *const c_char,
    /// DS4 / DS5 行为开关。
    pub ds_touchpad_plus_minus: bool,
    pub ds_capture_key: bool,
    /// 系统信息页四行文本（数字与单位在固件侧格式化）。
    pub firmware_version: *const c_char,
    pub heap_text: *const c_char,
    pub psram_text: *const c_char,
    pub battery_text: *const c_char,
    /// 调试页注入反馈高亮（-1 无，0-2 对应三个按钮）。
    pub debug_flash: i32,
    /// 弹窗：0 无、1 重启确认、2 关机确认、3 切手柄确认、4 切回串口后重启询问。
    pub dialog: i32,
    /// 全屏遮罩。
    pub powering_off: bool,
    pub rebooting: bool,
    /// 息屏时触摸整段跳过（画面不可见，触点只会误触看不见的控件）。
    pub screen_on: bool,
    /// 省电档（BLE 关闭）：界面按 12 fps 等效节拍轮询与推进动画。
    pub power_save: bool,
}

/// 逐帧渲染统计：面板提交耗时与渲染耗时分开记。
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub frames: u32,
    pub window_frames: u32,
    pub window_render_us: u64,
    pub window_flush_us: u64,
    pub window_damage_px: u64,
    pub max_render_us: u32,
    pub max_flush_us: u32,
}

/* 与 C 头文件的布局核对（偏移由 C 侧 _Static_assert 同步校验，见 docs/ARCHITECTURE.md 的显示通路）。 */
const _: () = {
    assert!(core::mem::size_of::<Touch>() == 4);
    assert!(core::mem::size_of::<Hooks>() == 8);
    assert!(core::mem::size_of::<UiState>() == 88);
    assert!(core::mem::align_of::<UiState>() == 4);
    assert!(core::mem::offset_of!(UiState, usb_role) == 16);
    assert!(core::mem::offset_of!(UiState, pad_family) == 24);
    assert!(core::mem::offset_of!(UiState, buttons) == 36);
    assert!(core::mem::offset_of!(UiState, controller_address) == 52);
    assert!(core::mem::offset_of!(UiState, firmware_version) == 60);
    assert!(core::mem::offset_of!(UiState, heap_text) == 64);
    assert!(core::mem::offset_of!(UiState, psram_text) == 68);
    assert!(core::mem::offset_of!(UiState, battery_text) == 72);
    assert!(core::mem::offset_of!(UiState, debug_flash) == 76);
    assert!(core::mem::offset_of!(UiState, dialog) == 80);
    assert!(core::mem::offset_of!(UiState, powering_off) == 84);
    assert!(core::mem::offset_of!(UiState, rebooting) == 85);
    assert!(core::mem::offset_of!(UiState, screen_on) == 86);
    assert!(core::mem::offset_of!(UiState, power_save) == 87);
    assert!(core::mem::size_of::<Stats>() == 40);
    assert!(core::mem::align_of::<Stats>() == 8);
    assert!(core::mem::offset_of!(Stats, window_render_us) == 8);
    assert!(core::mem::offset_of!(Stats, max_render_us) == 32);
    assert!(core::mem::offset_of!(Stats, max_flush_us) == 36);
};


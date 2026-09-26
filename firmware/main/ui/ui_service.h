#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 屏幕 UI 的 core 侧契约：固件核心对界面知道的全部内容。
 * 状态快照经 ui_service_fill_state 装配（UI 提供者每轮拉取），动作经
 * ui_service_handle_action 交回；生命周期入口由链接进来的 UI 提供者
 * （Slint 组件或无 UI 构建的空实现）实现，固件核心不依赖任何界面框架。
 * 状态与动作都在 UI 任务上下文交换，core 侧视图态因此不需要加锁。
 */

/** 界面状态快照：一次轮询填满，UI 侧按字段写入对应属性。
 *  布局与 ui/slint_ui/src/abi.rs 的镜像一致，改字段两侧同步。 */
typedef struct {
  /** 背光百分比（0-100）。 */
  int backlight;
  /** 电池百分比与充电标记。 */
  int battery_percent;
  /** 配对状态：0 idle、1 paired、2 advertising、3 scanning、4 pairing、5 connected。 */
  int pairing;
  /** 命令应答提示（0 无），取值见 ui/src/pages.slint 的 notice-label。 */
  int notice;
  /** USB 角色：0 device（串口）、1 host（手柄）。 */
  int usb_role;
  /** PC 是否连在串口上。 */
  bool pc_link;
  /** 物理手柄是否接入与家族（0 PAD、1 PS、2 XBOX、3 NS、4 STEAM）。 */
  bool pad_attached;
  int pad_family;
  /** 主机下发的玩家序号灯掩码。 */
  int player_led;
  /** 手柄操控屏幕模式是否生效。 */
  bool pad_ui_mode;
  /** UI 按键位（见 dp/dp_ui.h 的 DP_UI_BTN_*），未按下为 0。 */
  int buttons;
  /** OTA 阶段与百分比。 */
  int ota_phase;
  int ota_percent;
  /** 手柄配色命中款（0-3），未命中为 -1。 */
  int selected_colorway;
  /** 对外蓝牙地址（显示序），未同步时传 "--"。 */
  const char *controller_address;
  /** DS4 / DS5 行为开关。 */
  bool ds_touchpad_plus_minus;
  bool ds_capture_key;
  /** 系统信息页四行文本（数字与单位在固件侧格式化）。 */
  const char *firmware_version;
  const char *heap_text;
  const char *psram_text;
  const char *battery_text;
  /** 调试页注入反馈高亮（-1 无，0-2 对应三个按钮）。 */
  int debug_flash;
  /** 弹窗：0 无、1 重启确认、2 关机确认、3 切手柄确认、4 切回串口后重启询问。 */
  int dialog;
  /** 全屏遮罩。 */
  bool powering_off;
  bool rebooting;
  /** 息屏时触摸整段跳过（画面不可见，触点只会误触看不见的控件）。 */
  bool screen_on;
  /** 省电档（BLE 关闭）：界面按 12 fps 等效节拍轮询与推进动画。 */
  bool power_save;
} remapad_ui_state_t;

/** trace 命令不带帧数时的追踪长度。 */
#define REMAPAD_UI_TRACE_FRAMES_DEFAULT 60U

/* ---- 生命周期：固件核心调用，实现方是 UI 提供者（Slint 组件或无 UI 空实现） ---- */

/** 启动界面提供者（面板/触摸/背光初始化与渲染任务都在它内部）。 */
esp_err_t remapad_ui_start(void);

/** 请求一次实机截图（串口 shot 命令）：下一轮状态轮询把整屏画面回传给 PC。 */
void remapad_ui_request_shot(void);

/** 请求逐帧渲染统计（串口 trace 命令）：接下来 frames 帧每帧一行渲染/提交耗时。 */
void remapad_ui_request_trace(unsigned frames);

/* ---- core 侧装配与分发：UI 提供者在自己的任务上调用 ---- */

/** 复位视图态（提示/弹窗/就绪标记）：提供者启动时调一次。 */
void ui_service_reset(void);

/** 装配一轮状态快照：设备状态读数折算成界面属性，视图态一并合入。 */
void ui_service_fill_state(remapad_ui_state_t *state);

/** 界面动作分发（name 是 .slint 侧的动作名，value 是参数）。 */
void ui_service_handle_action(const char *name, int value);

/** 实时内存全景（串口 mem 命令的应答）：内部堆与 PSRAM 余量打到控制台出口。 */
void ui_service_print_mem(void);

#ifdef __cplusplus
}
#endif

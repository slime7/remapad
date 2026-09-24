#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ui_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Slint 屏幕 UI 的固件侧接口：状态快照进、动作回调出。
 * 状态快照结构 remapad_ui_state_t 由固件核心的 ui_service.h 契约定义，
 * 本头只补平台挂钩与界面生命周期入口。
 *
 * 状态与动作都在 UI 任务上下文交换（Slint 事件循环所在的那个任务），
 * 固件侧因此不需要加锁；数据面的高频报告不经过这里。
 */

/** 屏幕逻辑视口（与 .slint 根组件的宽高一致）。 */
#define REMAPAD_SLINT_VIEW_WIDTH 240
#define REMAPAD_SLINT_VIEW_HEIGHT 280

/** 状态快照回调（UI 任务上下文，每 50 ms 一次）。 */
typedef void (*remapad_ui_poll_fn)(remapad_ui_state_t *state, void *user);

/** 动作回调（UI 任务上下文）：name 是 .slint 侧的动作名，value 是参数。 */
typedef void (*remapad_ui_action_fn)(const char *name, int value, void *user);

/** 一次采样得到的触点，坐标为逻辑视口像素。 */
typedef struct {
    uint16_t x;
    uint16_t y;
} remapad_slint_touch_t;

/** 平台需要的硬件入口：面板提交与触摸采样都由固件侧提供。 */
typedef struct {
    /** 把一段 RGB565 小端像素写到面板窗口，阻塞到传输完成。 */
    esp_err_t (*transfer)(uint16_t *pixels, int x, int y, int width, int height);
    /** 采样当前触点，返回有效触点数。 */
    size_t (*touch_sample)(remapad_slint_touch_t *out, size_t capacity);
} remapad_slint_hooks_t;

/** 逐帧渲染统计：面板提交耗时与渲染耗时分开记，供串口 trace 与周期日志使用。 */
typedef struct {
    uint32_t frames;
    uint32_t window_frames;
    uint64_t window_render_us;
    uint64_t window_flush_us;
    uint64_t window_damage_px;
    uint32_t max_render_us;
    uint32_t max_flush_us;
} remapad_slint_stats_t;

/**
 * 启动 UI：创建平台与窗口、接好状态与动作回调，然后进入事件循环。
 * 本函数不返回（事件循环独占调用它的任务）。
 */
esp_err_t remapad_slint_ui_start(const remapad_slint_hooks_t *hooks, remapad_ui_poll_fn poll,
                                 remapad_ui_action_fn action, void *user);

/** 进入 Slint 事件循环：本函数不返回，独占调用它的任务。 */
void remapad_slint_ui_loop(void);

/** 把整屏当前画面按 RGB565 小端拷进 out（240 × 280 × 2 字节），截图通路使用。 */
void remapad_slint_ui_copy_frame(uint16_t *out);

/** 当前帧缓冲（RGB565 小端，视口全宽）；平台未就绪时为 NULL。 */
const uint16_t *remapad_slint_ui_frame(void);

/** 读取并清零一个统计窗口的逐帧数据。 */
void remapad_slint_ui_take_stats(remapad_slint_stats_t *out);

/** 息屏/亮屏开关：关闭时平台整段跳过触摸采样。 */
void remapad_slint_ui_set_touch_enabled(bool enabled);

/** 接下来 frames 帧逐帧打印渲染与提交耗时（串口 trace 命令；0 取默认长度）。 */
void remapad_slint_ui_trace_frames(unsigned frames);

#ifdef __cplusplus
}
#endif

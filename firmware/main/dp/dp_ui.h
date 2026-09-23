#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 手柄操控 UI：按住组合键（私有按键位 L1 + R1 + L3 + R3，四键同时按住再加保持时长）
 * 把主机手柄临时变成屏幕遥控器——捕获期间只向主机续发全松开的中性帧，方向键移动焦点、
 * 圆圈键等价于屏幕点按；再按一次同样的组合键退出。模式生效期间单独按住 L1 / R1 与十字键左右等价。
 * 数据面任务每周期调 dp_ui_frame 推进状态并发布按键，界面任务每帧轮询时读。
 */

/** 组合键：L1 + R1 + L3 + R3（私有按键位，四键同时按住才算命中）。 */
#define DP_UI_COMBO_MASK (PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_L3 | PAD_BTN_R3)

/** 组合键需持续按住多久才翻转模式（毫秒）。 */
#define DP_UI_COMBO_HOLD_MS 300U

/** 私有按键位 → 界面按键位（与各页的 focus-count-for 槽位对应）。 */
enum {
    DP_UI_BTN_UP = 0x0010,
    DP_UI_BTN_RIGHT = 0x0020,
    DP_UI_BTN_DOWN = 0x0040,
    DP_UI_BTN_LEFT = 0x0080,
    DP_UI_BTN_CIRCLE = 0x2000,
};

/** 模式翻转结果。 */
typedef enum {
    DP_UI_EVENT_NONE = 0,
    DP_UI_EVENT_ENTERED,
    DP_UI_EVENT_EXITED,
} dp_ui_event_t;

/** 纯逻辑状态：翻转判定只看组合键的按下沿与保持时长。 */
typedef struct {
    bool active;
    /** 上一次推进时组合键是否全按下；用于识别按下沿。 */
    bool captured;
    /** 本次按住已累计的时长（毫秒）。 */
    uint32_t held_ms;
    /** 本次按住是否已经翻转过了；松开后才允许再次翻转。 */
    bool fired;
} dp_ui_state_t;

/** 复位为「模式关闭、组合键未按下」。 */
void dp_ui_state_reset(dp_ui_state_t *state);

/** 组合键是否全部按下（纯函数）。 */
bool dp_ui_captured(uint32_t pad_buttons);

/** 私有按键位 → UI 按键位（纯函数）：十字键 + 圆圈键。 */
uint32_t dp_ui_map_buttons(uint32_t pad_buttons);

/** 私有按键位 → UI 按键位（纯函数）：肩键，按住 L1 / R1 等价于按左 / 右。
 *  组合键全按下或两肩键同按时不发方向（那是进出模式的动作）。 */
uint32_t dp_ui_map_nav(uint32_t pad_buttons);

/** 推进组合键判定（纯逻辑）：按住超过 DP_UI_COMBO_HOLD_MS 翻转一次模式。 */
dp_ui_event_t dp_ui_update(dp_ui_state_t *state, uint32_t pad_buttons, uint32_t dt_ms);

/* ---- 运行时：数据面任务写、界面任务与 CLI 读 ---- */

/** 数据面每周期调用：推进模式并把映射后的按键发布给 UI。 */
dp_ui_event_t dp_ui_frame(uint32_t pad_buttons, uint32_t dt_ms);

/** 手柄操控模式是否生效。 */
bool dp_ui_active(void);

/** 当前给界面的按键位（不在模式里恒为 0）。 */
uint32_t dp_ui_buttons(void);

/** 直接开关模式（串口 CLI 的 ui on|off）；返回是否发生变化。 */
bool dp_ui_set_active(bool on);

#ifdef __cplusplus
}
#endif

#include "dp_ui.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "remapad_dp_ui";

/** 运行时状态：dp_task 写、界面任务与 CLI 读，临界区保护。 */
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static dp_ui_state_t s_ui_state;
static uint32_t s_ui_buttons;

void dp_ui_state_reset(dp_ui_state_t *state)
{
  if (state == NULL) {
    return;
  }
  state->active = false;
  state->captured = false;
  state->held_ms = 0;
  state->fired = false;
  state->cross_down = false;
  state->cross_armed = false;
  state->cross_held_ms = 0;
  state->exit_wait_release = false;
}

bool dp_ui_captured(uint32_t pad_buttons)
{
  return (pad_buttons & DP_UI_COMBO_MASK) == DP_UI_COMBO_MASK;
}

uint32_t dp_ui_map_buttons(uint32_t pad_buttons)
{
  uint32_t buttons = 0;
  if ((pad_buttons & PAD_BTN_DPAD_UP) != 0) {
    buttons |= DP_UI_BTN_UP;
  }
  if ((pad_buttons & PAD_BTN_DPAD_DOWN) != 0) {
    buttons |= DP_UI_BTN_DOWN;
  }
  if ((pad_buttons & PAD_BTN_DPAD_LEFT) != 0) {
    buttons |= DP_UI_BTN_LEFT;
  }
  if ((pad_buttons & PAD_BTN_DPAD_RIGHT) != 0) {
    buttons |= DP_UI_BTN_RIGHT;
  }
  if ((pad_buttons & PAD_BTN_CIRCLE) != 0) {
    buttons |= DP_UI_BTN_CIRCLE;
  }
  return buttons;
}

uint32_t dp_ui_map_nav(uint32_t pad_buttons)
{
  /* 组合键以 L1 + R1 起手：四键同按（进出模式的那一刻）与两肩键同按
     * （组合键的前半段）都不发方向，其余时候按住 L1 / R1 就是左 / 右。 */
  if (dp_ui_captured(pad_buttons)) {
    return 0;
  }
  const bool l1 = (pad_buttons & PAD_BTN_L1) != 0;
  const bool r1 = (pad_buttons & PAD_BTN_R1) != 0;
  if (l1 == r1) {
    return 0;
  }
  return l1 ? DP_UI_BTN_LEFT : DP_UI_BTN_RIGHT;
}

dp_ui_event_t dp_ui_update(dp_ui_state_t *state, uint32_t pad_buttons, uint32_t dt_ms)
{
  if (state == NULL) {
    return DP_UI_EVENT_NONE;
  }
  /* 叉键按下沿：只有模式里按下的那一次按住算长按（进模式前就按着的不算），
     * 松开清掉本次按住的账。 */
  const bool cross = (pad_buttons & PAD_BTN_CROSS) != 0;
  if (!cross) {
    state->cross_down = false;
    state->cross_armed = false;
    state->cross_held_ms = 0;
    state->exit_wait_release = false;
  } else if (!state->cross_down) {
    state->cross_down = true;
    state->cross_armed = state->active;
    state->cross_held_ms = 0;
  }

  if (!dp_ui_captured(pad_buttons)) {
    /* 组合键没全按住：本次计时作废，松开后才允许下一次翻转。 */
    state->captured = false;
    state->held_ms = 0;
    state->fired = false;
  } else if (!state->fired) {
    if (!state->captured) {
      /* 按下沿：重新开始计时。 */
      state->captured = true;
      state->held_ms = 0;
    }
    state->held_ms += dt_ms;
    if (state->held_ms >= DP_UI_COMBO_HOLD_MS) {
      state->fired = true;
      state->active = !state->active;
      /* 模式翻了面：这一次叉键按住跟着作废，免得刚进模式就被它关掉。 */
      state->cross_armed = false;
      state->cross_held_ms = 0;
      return state->active ? DP_UI_EVENT_ENTERED : DP_UI_EVENT_EXITED;
    }
  }

  /* 叉键长按退出：一次按住只退一次，退出后等它松开。 */
  if (state->active && state->cross_armed) {
    state->cross_held_ms += dt_ms;
    if (state->cross_held_ms >= DP_UI_COMBO_HOLD_MS) {
      state->active = false;
      state->cross_armed = false;
      state->exit_wait_release = true;
      return DP_UI_EVENT_EXITED;
    }
  }
  return DP_UI_EVENT_NONE;
}

dp_ui_event_t dp_ui_frame(uint32_t pad_buttons, uint32_t dt_ms)
{
  dp_ui_event_t event = DP_UI_EVENT_NONE;
  portENTER_CRITICAL(&s_ui_mux);
  event = dp_ui_update(&s_ui_state, pad_buttons, dt_ms);
  /* 不在模式里恒发 0：UI 的按键只在捕获期间由手柄提供。 */
  s_ui_buttons = s_ui_state.active ? (dp_ui_map_buttons(pad_buttons) | dp_ui_map_nav(pad_buttons)) : 0;
  portEXIT_CRITICAL(&s_ui_mux);
  if (event != DP_UI_EVENT_NONE) {
    ESP_LOGI(TAG, "pad ui mode %s", event == DP_UI_EVENT_ENTERED ? "on" : "off");
  }
  return event;
}

bool dp_ui_active(void)
{
  bool active;
  portENTER_CRITICAL(&s_ui_mux);
  active = s_ui_state.active;
  portEXIT_CRITICAL(&s_ui_mux);
  return active;
}

bool dp_ui_muted(uint32_t pad_buttons)
{
  bool muted;
  portENTER_CRITICAL(&s_ui_mux);
  /* 叉键长按退出后它还按着：这时候放行会把这一次按住整段漏给主机，
     * 等到松开再恢复放行。 */
  muted = s_ui_state.active || s_ui_state.exit_wait_release || dp_ui_captured(pad_buttons);
  portEXIT_CRITICAL(&s_ui_mux);
  return muted;
}

uint32_t dp_ui_buttons(void)
{
  uint32_t buttons;
  portENTER_CRITICAL(&s_ui_mux);
  buttons = s_ui_buttons;
  portEXIT_CRITICAL(&s_ui_mux);
  return buttons;
}

bool dp_ui_set_active(bool on)
{
  bool changed = false;
  portENTER_CRITICAL(&s_ui_mux);
  if (s_ui_state.active != on) {
    s_ui_state.active = on;
    /* 手动开关跟组合键翻转同一个规矩：这一次叉键按住作废，
         * 免得模式翻面后拿上一次按住的余量把它关掉。 */
    s_ui_state.cross_armed = false;
    s_ui_state.cross_held_ms = 0;
    if (!on) {
      s_ui_buttons = 0;
    }
    changed = true;
  }
  portEXIT_CRITICAL(&s_ui_mux);
  if (changed) {
    ESP_LOGI(TAG, "pad ui mode %s (requested)", on ? "on" : "off");
  }
  return changed;
}

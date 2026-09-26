#include "ds_behavior.h"

/** 触摸板按下这一路在私有格式里的键位（见 pad_state.h）。 */
#define PAD_DS_TOUCHPAD_PRESS PAD_BTN_SHARE

/** 取「先触发」的半区：两半都有触点时比触发时刻，只有一半时就是它；
 *  都没有触点时返回假（位置未知）。 */
static bool first_half(const pad_ds_state_t *state, uint8_t *half)
{
  const uint32_t left = state->onset[PAD_TOUCH_LEFT];
  const uint32_t right = state->onset[PAD_TOUCH_RIGHT];
  if (left == 0 && right == 0) {
    return false;
  }
  if (left == 0 || (right != 0 && right < left)) {
    *half = PAD_TOUCH_RIGHT;
  } else {
    *half = PAD_TOUCH_LEFT;
  }
  return true;
}

/** 本次按下该发哪一位：先看「触摸板映射加减键」，位置取不到时退回
 *  「截图键」开关那一档。 */
static uint32_t decide_key(const pad_ds_state_t *state, const pad_ds_config_t *config)
{
  if (config->touchpad_plus_minus) {
    uint8_t half = PAD_TOUCH_LEFT;
    if (first_half(state, &half)) {
      return half == PAD_TOUCH_LEFT ? PAD_BTN_TOUCHPAD : PAD_BTN_OPT;
    }
  }
  return config->capture_key ? PAD_DS_TOUCHPAD_PRESS : PAD_BTN_TOUCHPAD;
}

void pad_ds_reset(pad_ds_state_t *state)
{
  *state = (pad_ds_state_t){ 0 };
}

void pad_ds_apply(pad_ds_state_t *state, const pad_ds_config_t *config, pad_state_t *pad)
{
  state->tick++;
  /* 触点触发时刻：每一路从「没有手指」变成「有手指」的那一拍记一次，
     * 按住期间不变——两半都有手指时按它比较先后。 */
  for (size_t i = 0; i < PAD_TOUCH_COUNT; i++) {
    const bool held = pad->touch[i].pressed;
    if (!held) {
      state->onset[i] = 0;
    } else if (!state->held[i]) {
      state->onset[i] = state->tick;
    }
    state->held[i] = held;
  }

  const bool pressed = (pad->buttons & PAD_DS_TOUCHPAD_PRESS) != 0;
  if (pad->family != PAD_FAMILY_PS || !pressed) {
    /* 非 PS 家族与松开的帧都不改写；松开即清掉本次按下的键位。 */
    state->mapped = 0;
    return;
  }
  if (state->mapped == 0) {
    state->mapped = decide_key(state, config);
  }
  pad->buttons = (pad->buttons & ~(uint32_t)PAD_DS_TOUCHPAD_PRESS) | state->mapped;
}

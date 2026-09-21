/**
 * DS4 / DS5 手柄行为（pad/ds_behavior.c）主机端用例：触摸板按下的跨采样语义——
 * 先触发的半区、按下期间锁位与位置取不到时的回落。
 */
#include "host_test.h"

#include <string.h>

#include "ds_behavior.h"
#include "pad_state.h"

/** 默认配置：触摸板映射加减键关、截图键开。 */
static const pad_ds_config_t kDefaults = {.touchpad_plus_minus = false, .capture_key = true};

static pad_state_t ps_pad(uint32_t buttons)
{
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.family = PAD_FAMILY_PS;
    pad.buttons = buttons;
    return pad;
}

/** 给某一半区放一根手指（位置取该半区中间）。 */
static void touch_half(pad_state_t *pad, uint8_t half)
{
    pad->touch[half].present = true;
    pad->touch[half].pressed = true;
    pad->touch[half].x = half == PAD_TOUCH_LEFT ? PAD_AXIS_CENTER / 2 : PAD_AXIS_CENTER * 2;
    pad->touch[half].y = PAD_AXIS_CENTER;
}

/** 默认设置：触摸板按下仍发截图，键位原样。 */
static void default_keeps_capture(void)
{
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &kDefaults, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_SHARE);
}

/** 截图键关掉：触摸板按下改发减号。 */
static void capture_off_maps_to_minus(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = false, .capture_key = false};
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(PAD_BTN_SHARE);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
}

/** 触摸板映射加减键：左半发减号、右半发加号，触摸板按下的截图位不再发。 */
static void touchpad_maps_halves(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = true};
    pad_ds_state_t state;

    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);

    pad_ds_reset(&state);
    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_OPT);
}

/** 两半都有手指时取先触发的那一半：先左后右发减号，先右后左发加号。 */
static void first_triggered_half_wins(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = true};
    pad_ds_state_t state;

    /* 第一拍：左半先落下（还没有按下触摸板）。 */
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(0);
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &config, &pad);
    /* 第二拍：右半也落下，同时按下触摸板。 */
    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_LEFT);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);

    /* 反向：右半先落下。 */
    pad_ds_reset(&state);
    pad = ps_pad(0);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_LEFT);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_OPT);
}

/** 按住期间先触发的那根手指抬手，键位不跟着换；松开后再按重新决定。 */
static void held_press_keeps_key(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = true};
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(0);
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &config, &pad);

    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_LEFT);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);

    /* 左半抬手：只剩右半，键位仍是按下那一刻定下的减号。 */
    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);

    /* 松开再按：这一次由右半决定，发加号。 */
    pad = ps_pad(0);
    pad_ds_apply(&state, &config, &pad);
    pad = ps_pad(PAD_BTN_SHARE);
    touch_half(&pad, PAD_TOUCH_RIGHT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_OPT);
}

/** 位置取不到（这一帧没有触摸数据）时退回截图键开关那一档。 */
static void missing_position_falls_back(void)
{
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = true};
    pad_state_t pad = ps_pad(PAD_BTN_SHARE);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_SHARE);

    pad_ds_reset(&state);
    config.capture_key = false;
    pad = ps_pad(PAD_BTN_SHARE);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_TOUCHPAD);
}

/** 非 PS 家族（Xbox Series 的分享键、NS 手柄）不受这两项设置影响。 */
static void other_families_untouched(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = false};
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(PAD_BTN_SHARE);
    pad.family = PAD_FAMILY_XBOX;
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)PAD_BTN_SHARE);
}

/** 映射只改触摸板那一位：同一帧里的其他键与摇杆原样保留。 */
static void other_buttons_keep(void)
{
    const pad_ds_config_t config = {.touchpad_plus_minus = true, .capture_key = true};
    pad_ds_state_t state;
    pad_ds_reset(&state);
    pad_state_t pad = ps_pad(PAD_BTN_SHARE | PAD_BTN_CROSS | PAD_BTN_L1);
    pad.axis[PAD_AXIS_LX] = 3000;
    touch_half(&pad, PAD_TOUCH_LEFT);
    pad_ds_apply(&state, &config, &pad);
    CHECK_EQ(pad.buttons, (uint32_t)(PAD_BTN_TOUCHPAD | PAD_BTN_CROSS | PAD_BTN_L1));
    CHECK_EQ(pad.axis[PAD_AXIS_LX], 3000);
}

HOST_TEST_SUITE(suite_ds_behavior, "pad/ds_behavior（DS4/DS5 触摸板按键行为）",
                {"默认设置下触摸板按下仍发截图", default_keeps_capture},
                {"截图键关掉后触摸板按下改发减号", capture_off_maps_to_minus},
                {"触摸板映射：左半减号、右半加号", touchpad_maps_halves},
                {"两半都有触点时取先触发的那一半", first_triggered_half_wins},
                {"按住期间先触发的手指抬手不改键位", held_press_keeps_key},
                {"位置取不到时退回截图键开关", missing_position_falls_back},
                {"非 PS 家族不受设置影响", other_families_untouched},
                {"映射只改触摸板那一位，同帧其他字段保留", other_buttons_keep});

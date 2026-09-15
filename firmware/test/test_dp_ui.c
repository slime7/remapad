/**
 * 手柄操控 UI 的组合键判定与按键映射（dp_ui.c）：组合键写错会表现为
 * 「按了没反应」或者「打着游戏突然手柄失灵」，映射写错则是「方向键按下去
 * 焦点不动 / 圆圈键确认不了」——两种都只能靠真机反复试，因此在主机上把
 * 按下沿、保持时长、松开后再触发与按键位对齐逐条钉住。
 */
#include "host_test.h"

#include "dp_ui.h"
#include "pad_state.h"

/** 一次 5ms 周期推进，返回事件。 */
static dp_ui_event_t tick(dp_ui_state_t *state, uint32_t buttons)
{
    return dp_ui_update(state, buttons, 5);
}

/** 按住组合键并推进到刚好越过保持阈值，返回期间发生的那次翻转。 */
static dp_ui_event_t hold_combo(dp_ui_state_t *state)
{
    dp_ui_event_t toggle = DP_UI_EVENT_NONE;
    const uint32_t ticks = DP_UI_COMBO_HOLD_MS / 5U + 1U;
    for (uint32_t i = 0; i < ticks; i++) {
        const dp_ui_event_t event = tick(state, DP_UI_COMBO_MASK);
        if (event != DP_UI_EVENT_NONE) {
            toggle = event;
        }
    }
    return toggle;
}

static void combo_needs_all_four_buttons(void)
{
    /* 少任何一个键都不算命中：漏键会让正常游戏的按键组合误开模式。 */
    const uint32_t parts[] = {
        PAD_BTN_L1,
        PAD_BTN_R1,
        PAD_BTN_L3,
        PAD_BTN_R3,
        PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_L3,
        PAD_BTN_L1 | PAD_BTN_R1 | PAD_BTN_L3 | PAD_BTN_CROSS,
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        CHECK(!dp_ui_captured(parts[i]));
        dp_ui_state_t state;
        dp_ui_state_reset(&state);
        CHECK_EQ(tick(&state, parts[i]), DP_UI_EVENT_NONE);
        CHECK(!state.active);
    }
    CHECK(dp_ui_captured(DP_UI_COMBO_MASK));
    /* 组合键之外的键同时按着不影响判定。 */
    CHECK(dp_ui_captured(DP_UI_COMBO_MASK | PAD_BTN_CIRCLE | PAD_BTN_DPAD_UP));
}

static void short_press_does_not_toggle(void)
{
    dp_ui_state_t state;
    dp_ui_state_reset(&state);
    /* 阈值以内按下再松开：一直是关着的，且不残留计时。 */
    for (uint32_t i = 0; i + 1 < DP_UI_COMBO_HOLD_MS / 5U; i++) {
        CHECK_EQ(tick(&state, DP_UI_COMBO_MASK), DP_UI_EVENT_NONE);
    }
    CHECK_EQ(tick(&state, 0), DP_UI_EVENT_NONE);
    CHECK(!state.active);
    CHECK_EQ(state.held_ms, 0);
    CHECK(!state.fired);

    /* 松开重按要走完整的保持时长：抖动不会攒够时间。 */
    CHECK_EQ(tick(&state, DP_UI_COMBO_MASK), DP_UI_EVENT_NONE);
    CHECK_EQ(tick(&state, 0), DP_UI_EVENT_NONE);
    CHECK_EQ(tick(&state, DP_UI_COMBO_MASK), DP_UI_EVENT_NONE);
    CHECK(!state.active);
}

static void hold_toggles_once_until_released(void)
{
    dp_ui_state_t state;
    dp_ui_state_reset(&state);
    CHECK_EQ(hold_combo(&state), DP_UI_EVENT_ENTERED);
    CHECK(state.active);
    CHECK_EQ(state.held_ms, DP_UI_COMBO_HOLD_MS);

    /* 继续按住不再翻转：一次按住只切换一次。 */
    for (uint32_t i = 0; i < 100; i++) {
        CHECK_EQ(tick(&state, DP_UI_COMBO_MASK), DP_UI_EVENT_NONE);
    }
    CHECK(state.active);

    /* 松开后再按同样的时长：退出模式。 */
    CHECK_EQ(tick(&state, 0), DP_UI_EVENT_NONE);
    CHECK_EQ(hold_combo(&state), DP_UI_EVENT_EXITED);
    CHECK(!state.active);
}

static void map_covers_dpad_and_circle_only(void)
{
    CHECK_EQ(dp_ui_map_buttons(0), 0U);
    CHECK_EQ(dp_ui_map_buttons(PAD_BTN_DPAD_UP), (uint32_t)DP_UI_BTN_UP);
    CHECK_EQ(dp_ui_map_buttons(PAD_BTN_DPAD_DOWN), (uint32_t)DP_UI_BTN_DOWN);
    CHECK_EQ(dp_ui_map_buttons(PAD_BTN_DPAD_LEFT), (uint32_t)DP_UI_BTN_LEFT);
    CHECK_EQ(dp_ui_map_buttons(PAD_BTN_DPAD_RIGHT), (uint32_t)DP_UI_BTN_RIGHT);
    CHECK_EQ(dp_ui_map_buttons(PAD_BTN_CIRCLE), (uint32_t)DP_UI_BTN_CIRCLE);
    /* 这一支只管十字键与圆圈：L1 / R1 走 dp_ui_map_nav，其余按键（L3 / R3、
     * 面键、组合键本身）都不该移动焦点。 */
    const uint32_t others = DP_UI_COMBO_MASK | PAD_BTN_CROSS | PAD_BTN_OPT | PAD_BTN_HOME |
                            PAD_BTN_TRIANGLE | PAD_BTN_SQUARE;
    CHECK_EQ(dp_ui_map_buttons(others), 0U);
}

static void shoulder_buttons_act_as_left_right(void)
{
    /* 模式里按住 L1 / R1 等价于按左 / 右：切底栏不必把拇指挪回十字键。 */
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_L1), (uint32_t)DP_UI_BTN_LEFT);
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_R1), (uint32_t)DP_UI_BTN_RIGHT);
    /* 同一个肩键与十字键一起按着：肩键这一路仍然算一次方向。 */
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_L1 | PAD_BTN_DPAD_DOWN), (uint32_t)DP_UI_BTN_LEFT);
}

static void combo_hold_emits_no_direction(void)
{
    /* 组合键以 L1 + R1 起手：进出模式的那一刻不能顺带发一次左右，否则进出
     * 模式都会把底栏的选中项挪走一格。 */
    CHECK_EQ(dp_ui_map_nav(DP_UI_COMBO_MASK), 0U);
    CHECK_EQ(dp_ui_map_nav(DP_UI_COMBO_MASK | PAD_BTN_DPAD_RIGHT), 0U);
    /* 两肩键同按是组合键的前半段（等着补上 L3 / R3），同样不发方向。 */
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_L1 | PAD_BTN_R1), 0U);
    /* 其余按键不参与：单按 L3 / R3 或面键都不发方向。 */
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_L3), 0U);
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_R3), 0U);
    CHECK_EQ(dp_ui_map_nav(PAD_BTN_CROSS), 0U);
    CHECK_EQ(dp_ui_map_nav(0), 0U);
}

static void runtime_publishes_shoulder_direction_in_mode(void)
{
    /* 运行时发布：模式里肩键进按键位，退出那一周期立刻归零。 */
    CHECK(dp_ui_set_active(true));
    dp_ui_frame(PAD_BTN_L1, 5);
    CHECK_EQ(dp_ui_buttons(), (uint32_t)DP_UI_BTN_LEFT);
    dp_ui_frame(PAD_BTN_R1 | PAD_BTN_CIRCLE, 5);
    CHECK_EQ(dp_ui_buttons(), (uint32_t)(DP_UI_BTN_RIGHT | DP_UI_BTN_CIRCLE));
    dp_ui_frame(DP_UI_COMBO_MASK, DP_UI_COMBO_HOLD_MS);
    CHECK(!dp_ui_active());
    CHECK_EQ(dp_ui_buttons(), 0U);
}

static void runtime_publishes_buttons_only_in_mode(void)
{
    /* 运行时入口（dp_task 写、owner task 读）：不在模式里恒为 0，进入模式后
     * 跟随手柄状态，退出后立刻归零——UI 的按键不会留在上一位。 */
    dp_ui_set_active(false);
    dp_ui_frame(PAD_BTN_DPAD_RIGHT | DP_UI_COMBO_MASK, 5);
    CHECK_EQ(dp_ui_buttons(), 0U);
    CHECK(!dp_ui_active());

    CHECK(dp_ui_set_active(true));
    CHECK(!dp_ui_set_active(true));
    CHECK(dp_ui_active());
    dp_ui_frame(PAD_BTN_DPAD_RIGHT | PAD_BTN_CIRCLE, 5);
    CHECK_EQ(dp_ui_buttons(), (uint32_t)(DP_UI_BTN_RIGHT | DP_UI_BTN_CIRCLE));
    /* 组合键按住时同步翻转回关：同一周期内按键即归零。 */
    dp_ui_frame(DP_UI_COMBO_MASK, DP_UI_COMBO_HOLD_MS);
    CHECK(!dp_ui_active());
    CHECK_EQ(dp_ui_buttons(), 0U);
}

HOST_TEST_SUITE(suite_dp_ui, "dp_ui 手柄操控 UI",
                {"组合键必须四键齐按才算命中", combo_needs_all_four_buttons},
                {"组合键短按不翻转，抖动不攒时间", short_press_does_not_toggle},
                {"组合键按住翻转一次，松开后才能再翻转", hold_toggles_once_until_released},
                {"只有十字键与圆圈键进 UI 按键位", map_covers_dpad_and_circle_only},
                {"按住 L1 / R1 与按左 / 右等价", shoulder_buttons_act_as_left_right},
                {"组合键与两肩键同按都不发方向", combo_hold_emits_no_direction},
                {"模式里肩键发布左右按键位，退出即归零", runtime_publishes_shoulder_direction_in_mode},
                {"模式外不发布 UI 按键，退出即归零", runtime_publishes_buttons_only_in_mode});

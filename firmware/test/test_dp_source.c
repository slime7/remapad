/**
 * 数据面输入源合成（dp_source.c）：多路输入的叠加规则错了，会表现为
 * 「插着手柄时摇杆漂移」或「调试注入的按键卡住不放」这类只在真机上出现的
 * 问题。合成与注入都是纯逻辑，适合在主机上逐个规则钉住。
 *
 * 注意：源注册表是进程级静态状态，本文件的用例按下面的顺序执行，
 * 新增用例时不要假设注册表是空的。
 */
#include "host_test.h"

#include <string.h>

#include "dp_source.h"
#include "ns2_state.h"

/* 由 firmware/test/support/stubs/ns2_output_stub.c 提供。 */
void host_test_set_nfc_state(uint8_t value);

static void primary_source(ns2_controller_state_t *state)
{
    state->buttons = NS2_BTN_A;
    state->stick_lx = 0x111;
    state->stick_ly = 0x222;
    state->stick_rx = 0x333;
    state->stick_ry = 0x444;
    state->battery_level = 7;
    state->battery_mv = 0x0BB8;
    state->external_power = true;
    state->rumble_enabled = true;
}

/** 后续源只有按键能生效：摇杆与电源字段应当被忽略。 */
static void secondary_source(ns2_controller_state_t *state)
{
    state->buttons = NS2_BTN_B;
    state->stick_lx = 0x999;
    state->stick_ly = 0x999;
    state->stick_rx = 0x999;
    state->stick_ry = 0x999;
    state->battery_level = 1;
    state->battery_mv = 0x0001;
    state->external_power = false;
    state->rumble_enabled = false;
}

static void tertiary_source(ns2_controller_state_t *state)
{
    state->buttons = NS2_BTN_DPAD_UP;
}

static const dp_source_t SOURCE_PRIMARY = {"测试主源", primary_source};
static const dp_source_t SOURCE_SECONDARY = {"测试次源", secondary_source};
static const dp_source_t SOURCE_TERTIARY = {"测试第三源", tertiary_source};
static const dp_source_t SOURCE_INVALID = {"无采样函数", NULL};

static void composition_rules(void)
{
    ns2_state_defaults(&(ns2_controller_state_t){0});

    /* 空注册表：全默认值，摇杆居中。 */
    ns2_controller_state_t state;
    host_test_set_nfc_state(0x02);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, 0);
    CHECK_EQ(state.stick_lx, NS2_STICK_CENTER);
    CHECK_EQ(state.nfc_state, 0x02); /* NFC 状态来自输出模块，不是输入源 */

    dp_source_register(&SOURCE_PRIMARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, NS2_BTN_A);
    CHECK_EQ(state.stick_lx, 0x111);
    CHECK_EQ(state.stick_ry, 0x444);
    CHECK_EQ(state.battery_level, 7);
    CHECK_EQ(state.battery_mv, 0x0BB8);
    CHECK(state.external_power);
    CHECK(state.rumble_enabled);

    /* 第二个源只叠加按键：摇杆与电源字段仍是主源的。 */
    dp_source_register(&SOURCE_SECONDARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, (uint32_t)(NS2_BTN_A | NS2_BTN_B));
    CHECK_EQ(state.stick_lx, 0x111);
    CHECK_EQ(state.stick_rx, 0x333);
    CHECK_EQ(state.battery_level, 7);
    CHECK_EQ(state.battery_mv, 0x0BB8);
    CHECK(state.external_power);
    CHECK(state.rumble_enabled);

    /* 第三个源同理。 */
    dp_source_register(&SOURCE_TERTIARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, (uint32_t)(NS2_BTN_A | NS2_BTN_B | NS2_BTN_DPAD_UP));
    CHECK_EQ(state.stick_ly, 0x222);

    /* 采样回调为空的源必须被拒绝，否则每帧都会空指针崩溃。 */
    dp_source_register(&SOURCE_INVALID);
    ns2_controller_state_t after;
    dp_source_sample(&after);
    CHECK_EQ(after.buttons, state.buttons);
    CHECK_EQ(after.stick_lx, state.stick_lx);
}

static void registration_limit(void)
{
    /* 注册表上限 4：已经用掉 3 个，第 4 个是最后一个位置。 */
    static const dp_source_t fourth = {"测试第四源", tertiary_source};
    dp_source_register(&fourth);

    static const dp_source_t fifth = {"测试第五源", tertiary_source};
    dp_source_register(&fifth); /* 满员：记日志并忽略 */

    ns2_controller_state_t state;
    dp_source_sample(&state);
    /* 第四个源生效，第五个没有。 */
    CHECK_EQ(state.buttons, (uint32_t)(NS2_BTN_A | NS2_BTN_B | NS2_BTN_DPAD_UP));

    dp_source_inject(NS2_BTN_HOME, 100);
    dp_source_sample(&state);
    /* 注入叠加在合成结果之上。 */
    CHECK_EQ(state.buttons, (uint32_t)(NS2_BTN_A | NS2_BTN_B | NS2_BTN_DPAD_UP | NS2_BTN_HOME));
}

static void debug_injection_holds_then_releases(void)
{
    /* dp_task 周期 5ms：hold 10ms ⇒ 保持 2 次采样。 */
    dp_source_inject(NS2_BTN_HOME, 10);
    CHECK(dp_source_inject_active());

    ns2_controller_state_t state;
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_HOME, NS2_BTN_HOME);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_HOME, NS2_BTN_HOME);

    /* 保持期结束：按键不再出现，注入标记也清空。 */
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_HOME, 0);
    CHECK(!dp_source_inject_active());

    /* 过短的 hold 会被抬到至少一个任务周期，避免点不动。
     * 这里挑一个没有输入源产出的键（C），否则断言分不清是注入还是源给的。 */
    dp_source_inject(NS2_BTN_C, 0);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_C, NS2_BTN_C);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_C, 0);

    /* 上限为 60000ms（12000 次采样）：超过旧的 5s 上限仍按住，便于长按
     * 验证与主机 Grip 界面的组合确认。 */
    dp_source_inject(NS2_BTN_B, 60000);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_B, NS2_BTN_B);
    for (int i = 0; i < 1100; i++) {
        dp_source_sample(&state);
    }
    CHECK_EQ(state.buttons & NS2_BTN_B, NS2_BTN_B);
    CHECK(dp_source_inject_active());
    dp_source_inject_release();
}

static void debug_release_clears_injection(void)
{
    dp_source_inject(NS2_BTN_HOME, 60000);

    ns2_controller_state_t state;
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_HOME, NS2_BTN_HOME);

    /* 提前释放：注入标记与按键同时清空。 */
    dp_source_inject_release();
    CHECK(!dp_source_inject_active());
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & NS2_BTN_HOME, 0);
}

static void debug_stick_injection(void)
{
    ns2_controller_state_t state;

    /* 未设定时沿用输入源：主源给的四轴原样透传。 */
    dp_source_inject_stick_reset();
    dp_source_sample(&state);
    CHECK_EQ(state.stick_lx, 0x111);
    CHECK_EQ(state.stick_ly, 0x222);
    CHECK_EQ(state.stick_rx, 0x333);
    CHECK_EQ(state.stick_ry, 0x444);

    /* 只推左摇杆：左轴被覆盖，右轴仍是输入源的值。 */
    dp_source_inject_stick('l', NS2_STICK_MAX, NS2_STICK_CENTER);
    dp_source_sample(&state);
    CHECK_EQ(state.stick_lx, NS2_STICK_MAX);
    CHECK_EQ(state.stick_ly, NS2_STICK_CENTER);
    CHECK_EQ(state.stick_rx, 0x333);
    CHECK_EQ(state.stick_ry, 0x444);

    /* 再给右摇杆另一个值：两侧各自保持。 */
    dp_source_inject_stick('r', 0, 0x800);
    dp_source_sample(&state);
    CHECK_EQ(state.stick_lx, NS2_STICK_MAX);
    CHECK_EQ(state.stick_ly, NS2_STICK_CENTER);
    CHECK_EQ(state.stick_rx, 0);
    CHECK_EQ(state.stick_ry, 0x800);

    /* 超界钳制到 12 位上限。 */
    dp_source_inject_stick('l', 0xFFFF, 0xFFFF);
    dp_source_sample(&state);
    CHECK_EQ(state.stick_lx, NS2_STICK_MAX);
    CHECK_EQ(state.stick_ly, NS2_STICK_MAX);

    /* 回中：解除注入，四轴回到输入源的值。 */
    dp_source_inject_stick_reset();
    dp_source_sample(&state);
    CHECK_EQ(state.stick_lx, 0x111);
    CHECK_EQ(state.stick_ly, 0x222);
    CHECK_EQ(state.stick_rx, 0x333);
    CHECK_EQ(state.stick_ry, 0x444);
}

static void debug_key_lookup(void)
{
    uint32_t mask = 0;
    uint32_t hold_ms = 0;

    CHECK(dp_source_key_lookup("a", 1, &mask, &hold_ms));
    CHECK_EQ(mask, NS2_BTN_A);
    CHECK_EQ(hold_ms, 250);

    CHECK(dp_source_key_lookup("up", 2, &mask, &hold_ms));
    CHECK_EQ(mask, NS2_BTN_DPAD_UP);

    /* 组合键默认按更长时间保持，对应主机 Grip 界面的确认动作。 */
    CHECK(dp_source_key_lookup("lr", 2, &mask, &hold_ms));
    CHECK_EQ(mask, (uint32_t)(NS2_BTN_L | NS2_BTN_R));
    CHECK_EQ(hold_ms, 1000);

    CHECK(dp_source_key_lookup("ls", 2, &mask, &hold_ms));
    CHECK_EQ(mask, NS2_BTN_LSTICK);
    CHECK(dp_source_key_lookup("c", 1, &mask, &hold_ms));
    CHECK_EQ(mask, NS2_BTN_C);

    /* 未命中：未知名字、空名字、前缀都不能改写输出。 */
    mask = 0;
    CHECK(!dp_source_key_lookup("nope", 4, &mask, &hold_ms));
    CHECK_EQ(mask, 0);
    CHECK(!dp_source_key_lookup("", 0, &mask, &hold_ms));
    CHECK(!dp_source_key_lookup("aa", 2, &mask, &hold_ms));
}

HOST_TEST_SUITE(suite_dp_source, "dp_source",
                {"合成规则：主源拥有摇杆与电源，其余只叠按键", composition_rules},
                {"注册上限与调试注入叠加", registration_limit},
                {"调试注入按时长保持后自动释放", debug_injection_holds_then_releases},
                {"调试释放立即清空注入按键", debug_release_clears_injection},
                {"摇杆注入：左右独立设定、钳制与回中", debug_stick_injection},
                {"按键名表：命中、默认保持时长与未命中", debug_key_lookup});

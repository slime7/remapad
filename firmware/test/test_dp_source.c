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
#include "dp_ui.h"
#include "pad_state.h"

static void primary_source(pad_state_t *state)
{
    state->buttons = PAD_BTN_CIRCLE;
    state->axis[PAD_AXIS_LX] = 0x111;
    state->axis[PAD_AXIS_LY] = 0x222;
    state->axis[PAD_AXIS_RX] = 0x333;
    state->axis[PAD_AXIS_RY] = 0x444;
    state->trigger[PAD_TRIGGER_L2] = 0x555;
    state->battery_percent = 77;
    state->battery_present = true;
    state->charging = true;
    state->family = PAD_FAMILY_XBOX;
    state->caps = PAD_CAP_TRIGGER_ANALOG;
    state->headset_present = true;
    state->headset_mic = true;
    /* 同代透传的字段也归主源：漏拷会让原始报文体永远是空的。 */
    state->native_lang = PAD_LANG_NS2;
    state->native_identity = PAD_IDENTITY_PRO;
    state->raw_report_id = 0x09;
    state->raw_len = 3;
    state->raw[0] = 0x09;
    state->raw[1] = 0x2A;
    state->raw[2] = 0x80;
}

/** 后续源只有按键能生效：摇杆、扳机与设备字段应当被忽略。 */
static void secondary_source(pad_state_t *state)
{
    state->buttons = PAD_BTN_CROSS;
    state->axis[PAD_AXIS_LX] = 0x999;
    state->axis[PAD_AXIS_RX] = 0x999;
    state->trigger[PAD_TRIGGER_L2] = 0x999;
    state->battery_percent = 1;
    state->family = PAD_FAMILY_PS;
    state->caps = PAD_CAP_MOTION;
    state->headset_present = false;
    state->headset_mic = false;
    state->native_lang = PAD_LANG_NS1;
    state->raw_len = 1;
    state->raw[0] = 0x3F;
}

static void tertiary_source(pad_state_t *state)
{
    state->buttons = PAD_BTN_DPAD_UP;
}

static const dp_source_t SOURCE_PRIMARY = {"测试主源", primary_source};
static const dp_source_t SOURCE_SECONDARY = {"测试次源", secondary_source};
static const dp_source_t SOURCE_TERTIARY = {"测试第三源", tertiary_source};
static const dp_source_t SOURCE_INVALID = {"无采样函数", NULL};

static void composition_rules(void)
{
    /* 空注册表：全默认值，摇杆居中。 */
    pad_state_t state;
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, 0);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_CENTER);
    CHECK_EQ(state.family, PAD_FAMILY_UNKNOWN);

    dp_source_register(&SOURCE_PRIMARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, (uint32_t)PAD_BTN_CIRCLE);
    CHECK_EQ(state.axis[PAD_AXIS_LX], 0x111);
    CHECK_EQ(state.axis[PAD_AXIS_RY], 0x444);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], 0x555);
    CHECK_EQ(state.battery_percent, 77);
    CHECK(state.battery_present);
    CHECK(state.charging);
    CHECK_EQ(state.family, PAD_FAMILY_XBOX);
    CHECK_EQ(state.caps, (uint32_t)PAD_CAP_TRIGGER_ANALOG);
    /* 耳机状态与透传字段同样只认主源：它们在支路被覆盖会让主机看到不存在的耳机，
     * 或者把支路的报文当成主手柄的原始报文转发出去。 */
    CHECK(state.headset_present);
    CHECK(state.headset_mic);
    CHECK_EQ(state.native_lang, PAD_LANG_NS2);
    CHECK_EQ(state.native_identity, PAD_IDENTITY_PRO);
    CHECK_EQ(state.raw_len, 3);
    CHECK_EQ(state.raw[0], 0x09);
    CHECK_EQ(state.raw[2], 0x80);

    /* 第二个源只叠加按键：摇杆、扳机与设备字段仍是主源的。 */
    dp_source_register(&SOURCE_SECONDARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, (uint32_t)(PAD_BTN_CIRCLE | PAD_BTN_CROSS));
    CHECK_EQ(state.axis[PAD_AXIS_LX], 0x111);
    CHECK_EQ(state.axis[PAD_AXIS_RX], 0x333);
    CHECK_EQ(state.trigger[PAD_TRIGGER_L2], 0x555);
    CHECK_EQ(state.battery_percent, 77);
    CHECK_EQ(state.family, PAD_FAMILY_XBOX);
    CHECK(state.headset_present);
    CHECK_EQ(state.native_lang, PAD_LANG_NS2);
    CHECK_EQ(state.raw_len, 3);
    CHECK_EQ(state.raw[1], 0x2A);

    /* 第三个源同理。 */
    dp_source_register(&SOURCE_TERTIARY);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons, (uint32_t)(PAD_BTN_CIRCLE | PAD_BTN_CROSS | PAD_BTN_DPAD_UP));
    CHECK_EQ(state.axis[PAD_AXIS_LY], 0x222);

    /* 采样回调为空的源必须被拒绝，否则每帧都会空指针崩溃。 */
    dp_source_register(&SOURCE_INVALID);
    pad_state_t after;
    dp_source_sample(&after);
    CHECK_EQ(after.buttons, state.buttons);
    CHECK_EQ(after.axis[PAD_AXIS_LX], state.axis[PAD_AXIS_LX]);
}

static void registration_limit(void)
{
    /* 注册表上限 4：已经用掉 3 个，第 4 个是最后一个位置。 */
    static const dp_source_t fourth = {"测试第四源", tertiary_source};
    dp_source_register(&fourth);

    static const dp_source_t fifth = {"测试第五源", tertiary_source};
    dp_source_register(&fifth); /* 满员：记日志并忽略 */

    pad_state_t state;
    dp_source_sample(&state);
    /* 第四个源生效，第五个没有。 */
    CHECK_EQ(state.buttons, (uint32_t)(PAD_BTN_CIRCLE | PAD_BTN_CROSS | PAD_BTN_DPAD_UP));

    dp_source_inject(PAD_BTN_HOME, 100);
    dp_source_sample(&state);
    /* 注入叠加在合成结果之上。 */
    CHECK_EQ(state.buttons,
             (uint32_t)(PAD_BTN_CIRCLE | PAD_BTN_CROSS | PAD_BTN_DPAD_UP | PAD_BTN_HOME));
}

static void debug_injection_holds_then_releases(void)
{
    /* dp_task 周期 5ms：hold 10ms ⇒ 保持 2 次采样。 */
    dp_source_inject(PAD_BTN_HOME, 10);
    CHECK(dp_source_inject_active());

    pad_state_t state;
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_HOME, PAD_BTN_HOME);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_HOME, PAD_BTN_HOME);

    /* 保持期结束：按键不再出现，注入标记也清空。 */
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_HOME, 0);
    CHECK(!dp_source_inject_active());

    /* 过短的 hold 会被抬到至少一个任务周期，避免点不动。
     * 这里挑一个没有输入源产出的键（静音位），否则断言分不清是注入还是源给的。 */
    dp_source_inject(PAD_BTN_MUTE, 0);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_MUTE, PAD_BTN_MUTE);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_MUTE, 0);

    /* 上限为 60000ms（12000 次采样）：超过旧的 5s 上限仍按住，便于长按
     * 验证与主机 Grip 界面的组合确认。 */
    dp_source_inject(PAD_BTN_CROSS, 60000);
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_CROSS, PAD_BTN_CROSS);
    for (int i = 0; i < 1100; i++) {
        dp_source_sample(&state);
    }
    CHECK_EQ(state.buttons & PAD_BTN_CROSS, PAD_BTN_CROSS);
    CHECK(dp_source_inject_active());
    dp_source_inject_release();
}

static void debug_release_clears_injection(void)
{
    dp_source_inject(PAD_BTN_HOME, 60000);

    pad_state_t state;
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_HOME, PAD_BTN_HOME);

    /* 提前释放：注入标记与按键同时清空。 */
    dp_source_inject_release();
    CHECK(!dp_source_inject_active());
    dp_source_sample(&state);
    CHECK_EQ(state.buttons & PAD_BTN_HOME, 0);
}

static void debug_stick_injection(void)
{
    pad_state_t state;

    /* 未设定时沿用输入源：主源给的四轴原样透传。 */
    dp_source_inject_stick_reset();
    dp_source_sample(&state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], 0x111);
    CHECK_EQ(state.axis[PAD_AXIS_LY], 0x222);
    CHECK_EQ(state.axis[PAD_AXIS_RX], 0x333);
    CHECK_EQ(state.axis[PAD_AXIS_RY], 0x444);

    /* 只推左摇杆：左轴被覆盖，右轴仍是输入源的值。 */
    dp_source_inject_stick('l', PAD_AXIS_MAX, PAD_AXIS_CENTER);
    dp_source_sample(&state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MAX);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_RX], 0x333);
    CHECK_EQ(state.axis[PAD_AXIS_RY], 0x444);

    /* 再给右摇杆另一个值：两侧各自保持。 */
    dp_source_inject_stick('r', 0, 0x800);
    dp_source_sample(&state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MAX);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_CENTER);
    CHECK_EQ(state.axis[PAD_AXIS_RX], 0);
    CHECK_EQ(state.axis[PAD_AXIS_RY], 0x800);

    /* 超界钳制到 12 位上限。 */
    dp_source_inject_stick('l', 0xFFFF, 0xFFFF);
    dp_source_sample(&state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], PAD_AXIS_MAX);
    CHECK_EQ(state.axis[PAD_AXIS_LY], PAD_AXIS_MAX);

    /* 回中：解除注入，四轴回到输入源的值。 */
    dp_source_inject_stick_reset();
    dp_source_sample(&state);
    CHECK_EQ(state.axis[PAD_AXIS_LX], 0x111);
    CHECK_EQ(state.axis[PAD_AXIS_LY], 0x222);
    CHECK_EQ(state.axis[PAD_AXIS_RX], 0x333);
    CHECK_EQ(state.axis[PAD_AXIS_RY], 0x444);
}

static void debug_key_lookup(void)
{
    uint32_t mask = 0;
    uint32_t hold_ms = 0;

    /* 键名是 NS2 的 a（右侧），掩码落到私有格式的 ○。 */
    CHECK(dp_source_key_lookup("a", 1, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_CIRCLE);
    CHECK_EQ(hold_ms, 250);

    CHECK(dp_source_key_lookup("up", 2, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_DPAD_UP);

    CHECK(dp_source_key_lookup("ls", 2, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_L3);
    /* 键名 c 是 NS2 的 C 键，掩码落到私有格式的静音位（PS 的静音键）。 */
    CHECK(dp_source_key_lookup("c", 1, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_MUTE);
    /* Nintendo 叫法落到私有格式的位置语义键上。 */
    CHECK(dp_source_key_lookup("gl", 2, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_L4);
    CHECK(dp_source_key_lookup("home", 4, &mask, &hold_ms));
    CHECK_EQ(mask, PAD_BTN_HOME);
    /* 手柄操控 UI 的组合键：一次注入就是「按下组合键并松开」，保持时长必须
     * 盖过 dp_ui 的翻转阈值，否则注入了但模式不切。 */
    CHECK(dp_source_key_lookup("ui", 2, &mask, &hold_ms));
    CHECK_EQ(mask, (uint32_t)DP_UI_COMBO_MASK);
    CHECK(hold_ms > DP_UI_COMBO_HOLD_MS);

    /* 未命中：未知名字、空名字、前缀都不能改写输出。 */
    mask = 0;
    CHECK(!dp_source_key_lookup("nope", 4, &mask, &hold_ms));
    CHECK_EQ(mask, 0);
    CHECK(!dp_source_key_lookup("", 0, &mask, &hold_ms));
    CHECK(!dp_source_key_lookup("aa", 2, &mask, &hold_ms));
    /* 配对 L+R 不再是调试键：主机 Grip 页不再是配对入口，JoyCon 组合未配对
     * 期间由固件自动注入（见 ns2_adv_lr_step），面板上不再需要手动兜底。 */
    CHECK(!dp_source_key_lookup("lr", 2, &mask, &hold_ms));
    CHECK_EQ(mask, 0);
}

HOST_TEST_SUITE(suite_dp_source, "dp_source",
                {"合成规则：主源拥有摇杆与设备字段，其余只叠按键", composition_rules},
                {"注册上限与调试注入叠加", registration_limit},
                {"调试注入按时长保持后自动释放", debug_injection_holds_then_releases},
                {"调试释放立即清空注入按键", debug_release_clears_injection},
                {"摇杆注入：左右独立设定、钳制与回中", debug_stick_injection},
                {"按键名表：命中、默认保持时长与未命中", debug_key_lookup});

/**
 * ui_service（屏幕 UI 的 core 侧契约）主机端用例：动作到控制面命令的映射、
 * 弹窗/提示的视图态流转与状态快照装配；硬件状态源全部走替身。
 */
#include "host_test.h"

#include <string.h>

#include "app_config.h"
#include "ui_service.h"

/* 替身控制口（support/stubs/ui_service_deps_stub.c）。 */
void host_test_set_app_config(const app_config_t *config);
void host_test_set_battery(uint32_t voltage_mv, uint8_t percentage);
void host_test_set_backlight(uint8_t pct);
void host_test_set_ns2_pairing(bool registered, bool waiting, bool pairing_mode, bool advertising,
                               bool paired);
void host_test_set_ns2_player_leds(uint8_t leds);
void host_test_set_ns2_identity(bool valid);
void host_test_set_usb_input(bool attached, uint16_t vid, uint16_t pid);
void host_test_set_usb_role_host(bool host);
void host_test_set_pc_link(bool active, bool pc_connected);
void host_test_set_ota_progress(int phase, int percent);
size_t host_test_ota_ui_ready_count(void);
void host_test_ota_ui_ready_reset(void);
void host_test_js_bridge_reset(void);
size_t host_test_js_bridge_command_count(void);
const char *host_test_js_bridge_command(size_t index);
int host_test_js_bridge_last_brightness(void);

static remapad_ui_state_t s_state;

/** 每条用例从干净现场开始：视图态复位、命令捕获清零、状态源给默认读数。 */
static void begin_case(void)
{
    ui_service_reset();
    host_test_js_bridge_reset();
    host_test_ota_ui_ready_reset();
    host_test_set_battery(4123, 75);
    host_test_set_backlight(40);
    host_test_set_ns2_pairing(false, false, false, false, false);
    host_test_set_ns2_player_leds(0);
    host_test_set_ns2_identity(false);
    host_test_set_usb_input(false, 0, 0);
    host_test_set_usb_role_host(false);
    host_test_set_pc_link(false, false);
    host_test_set_ota_progress(0, 0);
    memset(&s_state, 0, sizeof(s_state));
}

/** 空白配置：配色四段全零、DS 行为取默认（触摸板映射关、截图键开）。 */
static void begin_case_with_default_config(void)
{
    app_config_t config;
    memset(&config, 0, sizeof(config));
    config.brightness = 40;
    config.screen_on = true;
    config.ds_capture_key = true;
    host_test_set_app_config(&config);
    begin_case();
}

static void brightness_actions_step_in_fives(void)
{
    begin_case();
    host_test_set_backlight(40);
    ui_service_handle_action("brightness-up", 0);
    CHECK_EQ(host_test_js_bridge_last_brightness(), 60);
    ui_service_handle_action("brightness-down", 0);
    CHECK_EQ(host_test_js_bridge_last_brightness(), 40);
    /* 两端钳位：亮度档位只有 20/40/60/80/100 五档。 */
    host_test_set_backlight(100);
    ui_service_handle_action("brightness-up", 0);
    CHECK_EQ(host_test_js_bridge_last_brightness(), 100);
    host_test_set_backlight(20);
    ui_service_handle_action("brightness-down", 0);
    CHECK_EQ(host_test_js_bridge_last_brightness(), 20);
    /* 亮度走直调 setter，不进命令队列。 */
    CHECK_EQ(host_test_js_bridge_command_count(), 0U);
}

static void colorway_action_writes_config_colors(void)
{
    begin_case_with_default_config();
    ui_service_handle_action("colorway", 2);
    CHECK_EQ(app_config_get()->body_color, 0xb9bec4U);
    CHECK_EQ(app_config_get()->button_color, 0x6e757cU);
    CHECK_EQ(app_config_get()->accent_color, 0xe6e6e6U);
    CHECK_EQ(app_config_get()->grip_color, 0x8a9096U);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.selected_colorway, 2);
    /* 越界索引不改配置。 */
    ui_service_handle_action("colorway", 4);
    ui_service_handle_action("colorway", -1);
    CHECK_EQ(app_config_get()->body_color, 0xb9bec4U);
}

static void link_actions_submit_commands_with_notice(void)
{
    begin_case();
    ui_service_handle_action("connect", 0);
    CHECK_EQ(host_test_js_bridge_command_count(), 1U);
    CHECK(strcmp(host_test_js_bridge_command(0), "{\"t\":\"connect\",\"id\":0}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 2);

    ui_service_handle_action("pair", 0);
    CHECK(strcmp(host_test_js_bridge_command(1), "{\"t\":\"startPairing\",\"id\":0}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 1);

    /* 调试键注入走同一条命令通路。 */
    ui_service_handle_action("debug-key", 1);
    CHECK(strcmp(host_test_js_bridge_command(2),
                 "{\"t\":\"debugKey\",\"id\":0,\"key\":\"home\"}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.debug_flash, 1);
}

static void disconnect_notice_depends_on_pairing_state(void)
{
    begin_case();
    /* 静默时断开：界面先看到 idle，再按断开给「已回静默」。 */
    ui_service_fill_state(&s_state);
    ui_service_handle_action("disconnect", 0);
    CHECK(strcmp(host_test_js_bridge_command(0), "{\"t\":\"disconnect\",\"id\":0}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 4);

    /* 配对流程中（等待配对/已注册）断开给「已断开」。 */
    host_test_set_ns2_pairing(false, true, false, false, false);
    ui_service_fill_state(&s_state);
    ui_service_handle_action("disconnect", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 3);
}

static void host_click_connects_only_when_idle_or_paired(void)
{
    begin_case();
    ui_service_handle_action("host-click", 0);
    CHECK_EQ(host_test_js_bridge_command_count(), 1U);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 2);

    /* 广播中再点底栏不算连接键：不动命令队列。 */
    host_test_set_ns2_pairing(false, false, false, true, false);
    ui_service_handle_action("host-click", 0);
    CHECK_EQ(host_test_js_bridge_command_count(), 1U);
}

static void power_dialogs_confirm_cancel_and_mask(void)
{
    begin_case();
    /* 三个确认弹窗先弹再确认；取消立即收窗。 */
    ui_service_handle_action("ask-reboot", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 1);
    ui_service_handle_action("dialog-cancel", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 0);

    ui_service_handle_action("ask-power-off", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 2);
    ui_service_handle_action("power-off", 0);
    CHECK(strcmp(host_test_js_bridge_command(0), "{\"t\":\"powerOff\",\"id\":0}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 0);
    CHECK(s_state.powering_off);

    ui_service_handle_action("ask-usb-host", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 3);
    ui_service_handle_action("reboot", 0);
    CHECK(strcmp(host_test_js_bridge_command(1), "{\"t\":\"reboot\",\"id\":0}") == 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 0);
    CHECK(s_state.rebooting);
}

static void usb_role_actions_submit_role_commands(void)
{
    begin_case();
    ui_service_handle_action("usb-host", 0);
    CHECK(strcmp(host_test_js_bridge_command(0),
                 "{\"t\":\"setUsbRole\",\"id\":0,\"role\":\"host\"}") == 0);
    ui_service_handle_action("usb-device", 0);
    CHECK(strcmp(host_test_js_bridge_command(1),
                 "{\"t\":\"setUsbRole\",\"id\":0,\"role\":\"device\"}") == 0);
}

static void ds_actions_flip_one_switch_each(void)
{
    begin_case_with_default_config();
    ui_service_handle_action("ds-touchpad", 0);
    CHECK(strcmp(host_test_js_bridge_command(0),
                 "{\"t\":\"setDsBehavior\",\"id\":0,\"config\":{\"touchpadPlusMinus\":true,"
                 "\"captureKey\":true}}") == 0);
    /* 每个动作只翻自己的开关，另一项保持配置当前值。 */
    ui_service_handle_action("ds-capture", 0);
    CHECK(strcmp(host_test_js_bridge_command(1),
                 "{\"t\":\"setDsBehavior\",\"id\":0,\"config\":{\"touchpadPlusMinus\":false,"
                 "\"captureKey\":false}}") == 0);
}

static void usb_role_switchback_raises_reboot_dialog(void)
{
    begin_case();
    host_test_set_usb_role_host(true);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.usb_role, 1);
    CHECK_EQ(s_state.dialog, 0);
    /* 从手柄切回串口：PHY 交还失败只有复位能恢复，界面问一次要不要重启。 */
    host_test_set_usb_role_host(false);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.dialog, 4);
}

static void pairing_transition_clears_notice(void)
{
    begin_case();
    ui_service_handle_action("connect", 0);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.notice, 2);
    /* 首轮配对读数落地不清提示（last_pairing 从哨兵 -1 起步）。 */
    CHECK_EQ(s_state.pairing, 0);
    CHECK_EQ(s_state.notice, 2);
    /* 配对状态真的流转了才清掉上一句命令提示。 */
    host_test_set_ns2_pairing(false, false, false, true, false);
    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.pairing, 2);
    CHECK_EQ(s_state.notice, 0);
}

static void fill_state_maps_device_snapshot(void)
{
    begin_case_with_default_config();
    host_test_set_battery(4123, 75);
    host_test_set_ns2_pairing(true, true, true, true, true);
    host_test_set_ns2_player_leds(0x06);
    host_test_set_ns2_identity(false);
    host_test_set_usb_input(true, 0x054C, 0x0CE6);
    host_test_set_pc_link(true, true);
    host_test_set_ota_progress(1, 42);

    ui_service_fill_state(&s_state);
    CHECK_EQ(s_state.backlight, 40);
    CHECK_EQ(s_state.battery_percent, 75);
    CHECK_EQ(s_state.pairing, 5);
    CHECK_EQ(s_state.usb_role, 0);
    CHECK(s_state.pc_link);
    CHECK(s_state.pad_attached);
    CHECK_EQ(s_state.pad_family, 1);
    CHECK_EQ(s_state.player_led, 0x06);
    CHECK_EQ(s_state.ota_phase, 1);
    CHECK_EQ(s_state.ota_percent, 42);
    CHECK(s_state.screen_on);
    CHECK(!s_state.pad_ui_mode);
    CHECK_EQ(s_state.buttons, 0);
    CHECK(strcmp(s_state.firmware_version, "1.2.3-test") == 0);
    CHECK(strcmp(s_state.heap_text, "200 / 400 KB") == 0);
    CHECK(strcmp(s_state.psram_text, "1.0 / 8 MB") == 0);
    CHECK(strcmp(s_state.battery_text, "75% · 4.12V") == 0);
    /* host 同步前地址显示占位符。 */
    CHECK(strcmp(s_state.controller_address, "--") == 0);
}

static void identity_mac_shows_display_order(void)
{
    begin_case();
    host_test_set_ns2_identity(true);
    ui_service_fill_state(&s_state);
    /* 显示序是存储序反转。 */
    CHECK(strcmp(s_state.controller_address, "33:22:11:35:E6:9C") == 0);
}

static void ui_ready_notified_from_second_poll(void)
{
    begin_case();
    ui_service_fill_state(&s_state);
    CHECK_EQ(host_test_ota_ui_ready_count(), 0U);
    /* 第二轮回调起每轮都报就绪：OTA 回滚门槛要的是「画面已经上屏」。 */
    ui_service_fill_state(&s_state);
    CHECK_EQ(host_test_ota_ui_ready_count(), 1U);
    ui_service_fill_state(&s_state);
    CHECK_EQ(host_test_ota_ui_ready_count(), 2U);
}

HOST_TEST_SUITE(suite_ui_service, "ui_service 屏幕 UI 契约",
                { "亮度加减按五档步进并在两端钳位", brightness_actions_step_in_fives },
                { "colorway 动作写入四段配色并命中预设", colorway_action_writes_config_colors },
                { "连接/配对/调试键映射命令与提示", link_actions_submit_commands_with_notice },
                { "断开提示按配对状态分流", disconnect_notice_depends_on_pairing_state },
                { "底栏连接键只在静默或已配对时发连接", host_click_connects_only_when_idle_or_paired },
                { "电源与 USB 弹窗：确认、取消与全屏遮罩", power_dialogs_confirm_cancel_and_mask },
                { "USB 角色动作映射角色命令", usb_role_actions_submit_role_commands },
                { "DS 开关动作各翻各的开关", ds_actions_flip_one_switch_each },
                { "切回串口时弹重启询问", usb_role_switchback_raises_reboot_dialog },
                { "配对状态流转清掉命令提示", pairing_transition_clears_notice },
                { "状态快照装配逐字段对上读数", fill_state_maps_device_snapshot },
                { "蓝牙地址按显示序展示", identity_mac_shows_display_order },
                { "UI 就绪从第二轮轮询起上报", ui_ready_notified_from_second_poll })

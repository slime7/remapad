/**
 * 屏幕 UI 契约的 core 侧实现：把固件各模块的当前值装配成状态快照、把界面
 * 动作折算成控制面命令；不依赖任何界面框架（Slint 的装配在 ui/slint_ui 组件里）。
 * 视图态（提示/弹窗/遮罩）只在本模块内流转，fill/handle 都在 UI 任务上调用、无需加锁。
 */
#include "ui_service.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "backlight.h"
#include "battery.h"
#include "ble_controller.h"
#include "ble_session.h"
#include "bridge/js_bridge.h"
#include "console_out.h"
#include "dp_power.h"
#include "dp_ui.h"
#include "input_link.h"
#include "ns2_identity.h"
#include "ota_session.h"
#include "pad_device.h"
#include "usb_input.h"
#include "usb_role.h"

static const char *TAG = "remapad_ui";

/** 调试页注入反馈的高亮时长。 */
#define REMAPAD_DEBUG_FLASH_US (150 * 1000)

/** 控制器配色预设：与 ui/src/pages.slint 的手柄页四款预设同值。 */
static const uint32_t s_colorways[4][4] = {
    { 0x232323U, 0xa0a0a0U, 0xe6e6e6U, 0x323232U }, /* 标准黑 */
    { 0x3a4045U, 0x9aa3abU, 0xc8cdd2U, 0x2b2f33U }, /* 枪灰黑 */
    { 0xb9bec4U, 0x6e757cU, 0xe6e6e6U, 0x8a9096U }, /* 银灰 */
    { 0x1e3b2aU, 0xc8a24aU, 0xc8a24aU, 0x16301fU }, /* 墨绿金 */
};

/** UI 侧动作改写的固件侧状态：都在 UI 任务上读写，不需要同步。 */
static struct {
    int dialog;
    int notice;
    bool rebooting;
    bool powering_off;
    int debug_flash;
    int64_t debug_flash_until_us;
    int last_pairing;
    int last_usb_role;
    int page;
    bool ui_ready_sent;
    char firmware_version[48];
    char heap_text[32];
    char psram_text[32];
    char battery_text[32];
    char controller_address[20];
} s_ui;

/** UI 六态配对模型：与 bridge 的 real_pairing_state 同口径（见 js_bridge.c）。 */
static int ui_pairing_state(void)
{
    if (ns2_session_host_registered()) {
        return 5;
    }
    if (ns2_session_waiting_pair()) {
        return 4;
    }
    if (ns2_session_pairing_mode_active()) {
        return 3;
    }
    if (ns2_session_advertising()) {
        return 2;
    }
    return ns2_session_paired() ? 1 : 0;
}

/** 手柄家族 token → 底栏标签档位（0 PAD、1 PS、2 XBOX、3 NS、4 STEAM）。 */
static int ui_pad_family(void)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    if (!usb_input_attached() || !usb_input_device_ids(&vid, &pid, NULL)) {
        return 0;
    }
    switch (pad_family_from_ids(vid, pid)) {
    case PAD_FAMILY_PS:
        return 1;
    case PAD_FAMILY_XBOX:
        return 2;
    case PAD_FAMILY_NS:
        return 3;
    case PAD_FAMILY_STEAM:
        return 4;
    default:
        return 0;
    }
}

/** 当前配色命中哪一款预设（未命中返回 -1）：UI 侧据此画选中指示环。 */
static int ui_selected_colorway(const app_config_t *config)
{
    for (int index = 0; index < 4; ++index) {
        if (config->body_color == s_colorways[index][0] &&
            config->button_color == s_colorways[index][1] &&
            config->accent_color == s_colorways[index][2] &&
            config->grip_color == s_colorways[index][3]) {
            return index;
        }
    }
    return -1;
}

/** 对外蓝牙地址：host 同步前为空串，界面显示占位符。 */
static void ui_controller_address(void)
{
    uint8_t mac[6];
    if (!ns2_session_identity_mac(NS2_ID_PRO, mac)) {
        snprintf(s_ui.controller_address, sizeof(s_ui.controller_address), "--");
        return;
    }
    ns2_mac_to_string(mac, s_ui.controller_address);
}

void ui_service_reset(void)
{
    s_ui.last_pairing = -1;
    s_ui.last_usb_role = -1;
    s_ui.debug_flash = -1;
    s_ui.dialog = 0;
    s_ui.notice = 0;
    s_ui.rebooting = false;
    s_ui.powering_off = false;
    s_ui.ui_ready_sent = false;
}

void ui_service_fill_state(remapad_ui_state_t *state)
{
    const app_config_t *config = app_config_get();
    const esp_app_desc_t *desc = esp_app_get_description();

    if (desc != NULL) {
        snprintf(s_ui.firmware_version, sizeof(s_ui.firmware_version), "%s", desc->version);
    }
    const uint32_t heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t heap_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(s_ui.heap_text, sizeof(s_ui.heap_text), "%u / %u KB",
             (unsigned)((heap_total - heap_free) / 1024U), (unsigned)(heap_total / 1024U));
    snprintf(s_ui.psram_text, sizeof(s_ui.psram_text), "%u.%u / %u MB",
             (unsigned)((psram_total - psram_free) / (1024U * 1024U)),
             (unsigned)(((psram_total - psram_free) / (1024U * 1024U / 10U)) % 10U),
             (unsigned)(psram_total / (1024U * 1024U)));
    snprintf(s_ui.battery_text, sizeof(s_ui.battery_text), "%u%% · %u.%02uV",
             (unsigned)battery_get_percentage(),
             (unsigned)(battery_get_voltage_mv() / 1000U),
             (unsigned)((battery_get_voltage_mv() % 1000U) / 10U));
    ui_controller_address();

    const int pairing = ui_pairing_state();
    /* 配对状态真的流转了就清掉上一句命令提示（界面只显示当前状态）。 */
    if (pairing != s_ui.last_pairing) {
        if (s_ui.last_pairing >= 0) {
            s_ui.notice = 0;
        }
        s_ui.last_pairing = pairing;
    }
    const int usb_role = usb_role_host_active() ? 1 : 0;
    /* 从手柄切回串口：内部 PHY 交还失败时只有复位能恢复，界面问一次要不要重启。 */
    if (usb_role == 0 && s_ui.last_usb_role == 1 && s_ui.dialog == 0) {
        s_ui.dialog = 4;
    }
    s_ui.last_usb_role = usb_role;

    int ota_phase = 0;
    int ota_percent = 0;
    ota_session_progress(&ota_phase, &ota_percent);
    if (s_ui.debug_flash >= 0 && esp_timer_get_time() >= s_ui.debug_flash_until_us) {
        s_ui.debug_flash = -1;
    }

    state->backlight = backlight_get();
    state->battery_percent = battery_get_percentage();
    state->pairing = pairing;
    state->notice = s_ui.notice;
    state->usb_role = usb_role;
    state->pc_link = input_link_active() && input_link_pc_connected();
    state->pad_attached = usb_input_attached();
    state->pad_family = ui_pad_family();
    state->player_led = ns2_session_player_leds();
    state->pad_ui_mode = dp_ui_active();
    state->buttons = (int)dp_ui_buttons();
    state->ota_phase = ota_phase;
    state->ota_percent = ota_percent;
    state->selected_colorway = ui_selected_colorway(config);
    state->controller_address = s_ui.controller_address;
    state->ds_touchpad_plus_minus = config->ds_touchpad_plus_minus;
    state->ds_capture_key = config->ds_capture_key;
    state->firmware_version = s_ui.firmware_version;
    state->heap_text = s_ui.heap_text;
    state->psram_text = s_ui.psram_text;
    state->battery_text = s_ui.battery_text;
    state->debug_flash = s_ui.debug_flash;
    state->dialog = s_ui.dialog;
    state->powering_off = s_ui.powering_off;
    state->rebooting = s_ui.rebooting;
    state->screen_on = config->screen_on;
    /* 省电档由 BLE 栈的开关决定：栈关着（未连接也未广播）就降节拍。 */
    state->power_save = dp_power_save_active(ble_controller_running());

    /* 首帧之后的第二轮回调才算 UI 就绪：OTA 健康门槛要的是「画面已经上屏」。 */
    if (!s_ui.ui_ready_sent) {
        s_ui.ui_ready_sent = true;
    } else {
        ota_session_notify_ui_ready();
    }
}

/** 亮度档位：与亮度页的 5 档一致（20/40/60/80/100）。 */
static int brightness_step_up(int current)
{
    static const int steps[5] = { 20, 40, 60, 80, 100 };
    for (size_t index = 0; index < 5U; ++index) {
        if (steps[index] > current) {
            return steps[index];
        }
    }
    return 100;
}

static int brightness_step_down(int current)
{
    static const int steps[5] = { 20, 40, 60, 80, 100 };
    for (size_t index = 5U; index > 0U; --index) {
        if (steps[index - 1U] < current) {
            return steps[index - 1U];
        }
    }
    return 20;
}

void ui_service_handle_action(const char *name, int value)
{
    const app_config_t *config = app_config_get();
    char command[160];

    if (strcmp(name, "brightness-up") == 0) {
        js_bridge_set_brightness(brightness_step_up(backlight_get()));
    } else if (strcmp(name, "brightness-down") == 0) {
        js_bridge_set_brightness(brightness_step_down(backlight_get()));
    } else if (strcmp(name, "colorway") == 0) {
        if (value >= 0 && value < 4) {
            app_config_set_controller_colors(s_colorways[value][0], s_colorways[value][1],
                                            s_colorways[value][2], s_colorways[value][3]);
            ESP_LOGI(TAG, "colorway -> %d", value);
        }
    } else if (strcmp(name, "connect") == 0) {
        s_ui.notice = 2;
        (void)js_bridge_submit_command("{\"t\":\"connect\",\"id\":0}");
    } else if (strcmp(name, "disconnect") == 0) {
        const int pairing = ui_pairing_state();
        s_ui.notice = (pairing == 4 || pairing == 5) ? 3 : 4;
        (void)js_bridge_submit_command("{\"t\":\"disconnect\",\"id\":0}");
    } else if (strcmp(name, "pair") == 0) {
        s_ui.notice = 1;
        (void)js_bridge_submit_command("{\"t\":\"startPairing\",\"id\":0}");
    } else if (strcmp(name, "host-click") == 0) {
        const int pairing = ui_pairing_state();
        if (pairing == 0 || pairing == 1) {
            s_ui.notice = 2;
            (void)js_bridge_submit_command("{\"t\":\"connect\",\"id\":0}");
        }
    } else if (strcmp(name, "ask-reboot") == 0) {
        s_ui.dialog = 1;
    } else if (strcmp(name, "ask-power-off") == 0) {
        s_ui.dialog = 2;
    } else if (strcmp(name, "ask-usb-host") == 0) {
        s_ui.dialog = 3;
    } else if (strcmp(name, "dialog-cancel") == 0) {
        s_ui.dialog = 0;
    } else if (strcmp(name, "reboot") == 0) {
        s_ui.dialog = 0;
        s_ui.rebooting = true;
        (void)js_bridge_submit_command("{\"t\":\"reboot\",\"id\":0}");
    } else if (strcmp(name, "power-off") == 0) {
        s_ui.dialog = 0;
        s_ui.powering_off = true;
        (void)js_bridge_submit_command("{\"t\":\"powerOff\",\"id\":0}");
    } else if (strcmp(name, "usb-host") == 0) {
        s_ui.dialog = 0;
        (void)js_bridge_submit_command("{\"t\":\"setUsbRole\",\"id\":0,\"role\":\"host\"}");
    } else if (strcmp(name, "usb-device") == 0) {
        (void)js_bridge_submit_command("{\"t\":\"setUsbRole\",\"id\":0,\"role\":\"device\"}");
    } else if (strcmp(name, "ds-touchpad") == 0) {
        snprintf(command, sizeof(command),
                 "{\"t\":\"setDsBehavior\",\"id\":0,\"config\":{\"touchpadPlusMinus\":%s,"
                 "\"captureKey\":%s}}",
                 config->ds_touchpad_plus_minus ? "false" : "true",
                 config->ds_capture_key ? "true" : "false");
        (void)js_bridge_submit_command(command);
    } else if (strcmp(name, "ds-capture") == 0) {
        snprintf(command, sizeof(command),
                 "{\"t\":\"setDsBehavior\",\"id\":0,\"config\":{\"touchpadPlusMinus\":%s,"
                 "\"captureKey\":%s}}",
                 config->ds_touchpad_plus_minus ? "true" : "false",
                 config->ds_capture_key ? "false" : "true");
        (void)js_bridge_submit_command(command);
    } else if (strcmp(name, "debug-key") == 0) {
        const char *key = value == 0 ? "a" : (value == 1 ? "home" : "ui");
        snprintf(command, sizeof(command), "{\"t\":\"debugKey\",\"id\":0,\"key\":\"%s\"}", key);
        (void)js_bridge_submit_command(command);
        s_ui.debug_flash = value;
        s_ui.debug_flash_until_us = esp_timer_get_time() + REMAPAD_DEBUG_FLASH_US;
    } else if (strcmp(name, "page") == 0) {
        s_ui.page = value;
    }
}

void ui_service_print_mem(void)
{
    char report[256];
    const int len = snprintf(report, sizeof(report),
                             "mem psram free=%u largest=%u min=%u internal free=%u largest=%u "
                             "min=%u\r\n",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (len > 0) {
        console_out_write(report, (size_t)len);
    }
}

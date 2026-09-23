#include "slint_host.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_config.h"
#include "backlight.h"
#include "battery.h"
#include "ble_session.h"
#include "boot_splash.h"
#include "bridge/js_bridge.h"
#include "console_out.h"
#include "dp_ui.h"
#include "ds_behavior.h"
#include "input_frame.h"
#include "input_link.h"
#include "ns2_identity.h"
#include "ota_session.h"
#include "pad_device.h"
#include "panel.h"
#include "slint_ui.h"
#include "touch.h"
#include "usb_input.h"
#include "usb_role.h"

static const char *TAG = "remapad_slint";

/** owner task 栈：Slint 事件循环 + 渲染都在它上面，栈放内部 RAM 以保证渲染速度。 */
#define REMAPAD_SLINT_TASK_STACK_BYTES (64U * 1024U)
#define REMAPAD_SLINT_TASK_NAME "remapad-slint"
#define REMAPAD_SLINT_TASK_PRIORITY 5
/** 与数据面任务同核：BLE 控制器与 NimBLE 主机栈都在 CPU0。 */
#define REMAPAD_SLINT_TASK_CORE 1
/** 持久化亮度缺失时的兜底值。 */
#define REMAPAD_BACKLIGHT_PCT_DEFAULT 40
/** 启动阶段权重（毫秒）：Slint 的启动是建窗与首帧，比 guest eval 快两个数量级。 */
#define REMAPAD_SLINT_STAGE_MS_COUNT 4
static const uint32_t REMAPAD_SLINT_STAGE_MS[REMAPAD_SLINT_STAGE_MS_COUNT] = {
    60,  /* panel + touch + platform */
    60,  /* App::create + 首轮状态 */
    80,  /* 首帧渲染与提交 */
    20,  /* 进入事件循环 */
};
/** 截图分块回传的等待上限。 */
#define REMAPAD_SHOT_TX_TIMEOUT_MS 200U
/** 调试页注入反馈的高亮时长。 */
#define REMAPAD_DEBUG_FLASH_US (150 * 1000)
/** 控制器配色预设：与 ui/src/pages/ControllerSettingsPage.tsx 的 COLORWAYS 同值。 */
static const uint32_t s_colorways[4][4] = {
    { 0x232323U, 0xa0a0a0U, 0xe6e6e6U, 0x323232U }, /* 标准黑 */
    { 0x3a4045U, 0x9aa3abU, 0xc8cdd2U, 0x2b2f33U }, /* 枪灰黑 */
    { 0xb9bec4U, 0x6e757cU, 0xe6e6e6U, 0x8a9096U }, /* 银灰 */
    { 0x1e3b2aU, 0xc8a24aU, 0xc8a24aU, 0x16301fU }, /* 墨绿金 */
};

static atomic_bool s_shot_requested;
static atomic_bool s_mem_requested;

/** 截图与内存全景的应答都在状态轮询里同步回传（下面的实现体在其后）。 */
static void remapad_ui_stream_shot(void);
static void remapad_ui_print_mem(void);

/** UI 侧动作改写的固件侧状态：都在 owner task 上读写，不需要同步。 */
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

/** 面板提交入口：Slint 平台把行带交给它，驱动负责字节序与 DMA 等待。 */
static esp_err_t ui_panel_transfer(uint16_t *pixels, int x, int y, int width, int height)
{
    return panel_transfer(pixels, x, y, width, height);
}

/** 触摸采样入口：息屏期间由平台整段跳过，这里只做一次采样。 */
static size_t ui_touch_sample(remapad_slint_touch_t *out, size_t capacity)
{
    touch_contact_t contacts[4];
    size_t count = touch_sample(contacts, capacity < 4U ? capacity : 4U);
    for (size_t index = 0; index < count; ++index) {
        out[index].x = contacts[index].x;
        out[index].y = contacts[index].y;
    }
    return count;
}

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

/** 一轮状态快照：把固件各模块的当前值折算成界面属性。 */
static void ui_poll(remapad_ui_state_t *state, void *user)
{
    (void)user;
    const app_config_t *config = app_config_get();
    const esp_app_desc_t *desc = esp_app_get_description();

    /* 控制面命令队列与配对状态机由本任务驱动（界面每帧轮询状态时顺带处理）。 */
    js_bridge_service();

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

    /* 首帧之后的第二轮回调才算 UI 就绪：OTA 健康门槛要的是「画面已经上屏」。 */
    if (!s_ui.ui_ready_sent) {
        s_ui.ui_ready_sent = true;
    } else {
        ota_session_notify_ui_ready();
    }

    if (atomic_exchange_explicit(&s_shot_requested, false, memory_order_relaxed)) {
        remapad_ui_stream_shot();
    }
    if (atomic_exchange_explicit(&s_mem_requested, false, memory_order_relaxed)) {
        remapad_ui_print_mem();
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

/** UI 动作分发：命令统一走桥接的命令队列，由 js_bridge_service 在同一任务上执行。 */
static void ui_action(const char *name, int value, void *user)
{
    (void)user;
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

/** 整屏回传：截图通路把当前帧缓冲按 200 字节分块交给 PC。 */
static void remapad_ui_stream_shot(void)
{
    if (!input_link_active()) {
        return;
    }
    const uint16_t *frame = remapad_slint_ui_frame();
    if (frame == NULL) {
        return;
    }
    const uint16_t width = REMAPAD_SLINT_VIEW_WIDTH;
    const uint16_t height = REMAPAD_SLINT_VIEW_HEIGHT;
    if (input_link_send_image_info(width, height, REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "shot info frame dropped");
        return;
    }
    const uint8_t *bytes = (const uint8_t *)frame;
    const uint32_t total = (uint32_t)width * height * sizeof(uint16_t);
    uint32_t offset = 0;
    while (offset < total) {
        size_t piece = total - offset;
        if (piece > INPUT_FRAME_IMAGE_CHUNK_MAX) {
            piece = INPUT_FRAME_IMAGE_CHUNK_MAX;
        }
        if (input_link_send_image_data(offset, bytes + offset, piece,
                                       REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "shot aborted at %u/%u bytes", (unsigned)offset, (unsigned)total);
            return;
        }
        offset += (uint32_t)piece;
    }
    if (input_link_send_image_end(total, REMAPAD_SHOT_TX_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "shot end frame dropped");
        return;
    }
    ESP_LOGI(TAG, "shot sent: %ux%u (%u bytes)", (unsigned)width, (unsigned)height,
             (unsigned)total);
}

/** 实时内存全景（串口 mem 命令的应答）。 */
static void remapad_ui_print_mem(void)
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

void remapad_ui_request_shot(void)
{
    atomic_store_explicit(&s_shot_requested, true, memory_order_relaxed);
}

void remapad_ui_request_mem(void)
{
    atomic_store_explicit(&s_mem_requested, true, memory_order_relaxed);
}

void remapad_ui_request_trace(unsigned frames)
{
    if (frames == 0U) {
        frames = REMAPAD_UI_TRACE_FRAMES_DEFAULT;
    }
    remapad_slint_ui_trace_frames(frames);
}

static void slint_owner_task(void *opaque)
{
    (void)opaque;
    /* 面板与触摸初始化失败不阻断启动：面板失败时 Slint 仍渲染进 PSRAM。 */
    if (panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
    }
    if (touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed");
    }
    if (backlight_init() != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed");
    }
    const uint8_t brightness =
            app_config_get()->brightness != 0U ? app_config_get()->brightness
                                               : REMAPAD_BACKLIGHT_PCT_DEFAULT;
    /* 面板就绪后立刻画启动画面并点亮背光：Slint 建窗与首帧期间屏幕已有内容。 */
    if (boot_splash_begin(brightness, REMAPAD_SLINT_STAGE_MS, REMAPAD_SLINT_STAGE_MS_COUNT) !=
        ESP_OK) {
        ESP_LOGW(TAG, "boot splash unavailable");
    }

    const remapad_slint_hooks_t hooks = {
        .transfer = ui_panel_transfer,
        .touch_sample = ui_touch_sample,
    };
    if (remapad_slint_ui_start(&hooks, ui_poll, ui_action, NULL) != ESP_OK) {
        boot_splash_fail();
        ESP_LOGE(TAG, "slint ui start failed (internal=%u largest=%u psram=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelete(NULL);
        return;
    }
    /* 首帧之前交出启动画面：Slint 的第一帧是全屏重画，动画任务再画会打架。 */
    boot_splash_end();
    ESP_LOGI(TAG, "slint ui ready on CPU%d", (int)xPortGetCoreID());
    remapad_slint_ui_loop();
    vTaskDelete(NULL);
}

esp_err_t remapad_slint_start(void)
{
    esp_err_t bridge_result = js_bridge_init();
    if (bridge_result != ESP_OK) {
        ESP_LOGE(TAG, "js_bridge init failed: %s", esp_err_to_name(bridge_result));
        return bridge_result;
    }
    atomic_init(&s_shot_requested, false);
    atomic_init(&s_mem_requested, false);
    s_ui.last_pairing = -1;
    s_ui.last_usb_role = -1;
    s_ui.debug_flash = -1;
    s_ui.dialog = 0;
    s_ui.notice = 0;
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            slint_owner_task, REMAPAD_SLINT_TASK_NAME, REMAPAD_SLINT_TASK_STACK_BYTES, NULL,
            REMAPAD_SLINT_TASK_PRIORITY, NULL, REMAPAD_SLINT_TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "owner task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

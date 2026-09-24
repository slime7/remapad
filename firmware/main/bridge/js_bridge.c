#include "js_bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "backlight.h"
#include "app_config.h"
#include "battery.h"
#include "ble_controller.h"
#include "ble_session.h"
#include "dp_plane.h"
#include "dp_ui.h"
#include "input_link.h"
#include "ns2_identity.h"
#include "pad_device.h"
#include "pad_state.h"
#include "pwr_key.h"
#include "usb_input.h"
#include "usb_role.h"

static const char *TAG = "remapad_bridge";

#define REMAPAD_CHIP_NAME "ESP32-S3"
#define REMAPAD_BRIDGE_CMD_MAX 256
#define REMAPAD_BRIDGE_QUEUE_LEN 8
/** 事件 JSON 缓冲：systemStatus 是最大的一条，字段取现实中上界约 330 字节
 *  （含 playerLed），再加上 post_event_json 的调用包装约 50 字节；320 会在
 *  长时间运行后（uptimeMs 位数增长）截断，取 384 留出余量。 */
#define REMAPAD_EVENT_MAX 384

/** 重启前留出的应答时间：先让 UI 收到 rebooting 再重启。 */
#define REMAPAD_REBOOT_DELAY_US (150 * 1000LL)

/** 关机时序：先让 UI 收到应答，再释放电源锁存；释放后仍存活说明锁存被
 *  外部供电旁路（USB 供电），此时恢复锁存并如实回报 UI。 */
#define REMAPAD_POWER_OFF_DELAY_US (200 * 1000LL)
#define REMAPAD_POWER_OFF_VERIFY_US (1500 * 1000LL)

/** 外部命令/事件槽：PWR 按键与串口 CLI 等非 owner task 上下文的入口。
 * 外部任务只把字符串拷进队列，js_bridge_service 在 owner task 上取出处理。 */
#define REMAPAD_EXT_MSG_LEN 160
#define REMAPAD_EXT_QUEUE_LEN 4

typedef struct {
    char data[REMAPAD_EXT_MSG_LEN];
} ext_msg_slot_t;

typedef struct {
    char data[REMAPAD_BRIDGE_CMD_MAX];
} bridge_cmd_slot_t;

static struct {
    bridge_cmd_slot_t queue[REMAPAD_BRIDGE_QUEUE_LEN];
    size_t queue_head;
    size_t queue_len;
    QueueHandle_t ext_cmds;
    QueueHandle_t ext_events;
    const char *last_pairing_state; /* 字面量常量指针，用于变化检测。 */
    /** 上次上报给 UI 的玩家灯掩码；-1 表示尚未上报（0 是有效值：四格全灭）。 */
    int last_player_led;
    /** 上次上报给 UI 的手柄操控模式（两侧开机都视为关闭，变化才广播）。 */
    bool last_pad_ui_mode;
    /** 上次上报给 UI 的直插手柄接入状态；-1 表示尚未上报（开机补一次）。 */
    int last_pad_attached;
    /** 上次上报给 UI 的 PC 串口接入状态；-1 表示尚未上报（开机补一次）。 */
    int last_pc_connected;
    bool usb_role_host; /* 当前 USB 角色：true = 端口交给 OTG host（直插手柄）。 */
    int64_t reboot_at_us;
    bool reboot_pending;
    /** 关机阶段：0 空闲，1 待释放锁存，2 待确认是否已断电。 */
    int64_t power_off_at_us;
    uint8_t power_off_stage;
} s_bridge;

esp_err_t js_bridge_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.last_player_led = -1;
    s_bridge.last_pad_attached = -1;
    s_bridge.last_pc_connected = -1;
    s_bridge.ext_cmds = xQueueCreate(REMAPAD_EXT_QUEUE_LEN, sizeof(ext_msg_slot_t));
    s_bridge.ext_events = xQueueCreate(REMAPAD_EXT_QUEUE_LEN, sizeof(ext_msg_slot_t));
    if (s_bridge.ext_cmds == NULL || s_bridge.ext_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* USB 角色只在内存里生效（开机恒为串口），手柄身份从持久化配置恢复。 */
    s_bridge.usb_role_host = app_config_get()->usb_role == APP_CONFIG_USB_HOST;
    ns2_session_set_colors(app_config_get()->body_color, app_config_get()->button_color,
                           app_config_get()->accent_color, app_config_get()->grip_color);
    return ESP_OK;
}

esp_err_t js_bridge_enqueue(const char *cmd_json)
{
    if (cmd_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(cmd_json) >= REMAPAD_BRIDGE_CMD_MAX) {
        ESP_LOGW(TAG, "cmd too long, dropping: %.64s", cmd_json);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_bridge.queue_len >= REMAPAD_BRIDGE_QUEUE_LEN) {
        ESP_LOGW(TAG, "cmd queue full, dropping: %.64s", cmd_json);
        return ESP_ERR_NO_MEM;
    }
    const size_t tail = (s_bridge.queue_head + s_bridge.queue_len) % REMAPAD_BRIDGE_QUEUE_LEN;
    bridge_cmd_slot_t *slot = &s_bridge.queue[tail];
    strcpy(slot->data, cmd_json);
    s_bridge.queue_len++;
    return ESP_OK;
}

/** 事件只记日志：屏幕界面不接事件通道，状态由 UI 任务轮询取得。 */
static void post_event_json(const char *event_json)
{
    ESP_LOGD(TAG, "event: %s", event_json);
}

/* 命令 JSON 由 UI 侧 driver 的 JSON.stringify 生成、字段固定，这里按
 * "键":值 直接匹配；协议扩展若引入新字段形态，请同步这里的解析。 */
static bool cmd_has(const char *cmd, const char *key_value)
{
    return strstr(cmd, key_value) != NULL;
}

static int cmd_id(const char *cmd)
{
    const char *id = strstr(cmd, "\"id\":");
    return id != NULL ? atoi(id + 5) : 0;
}

static int cmd_number(const char *cmd, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *value = strstr(cmd, needle);
    if (value == NULL) {
        return -1;
    }
    return atoi(value + strlen(needle));
}

/** "key":"value" 形态的字符串字段；found 表示键存在。 */
static const char *cmd_string(const char *cmd, const char *key, size_t *len)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *value = strstr(cmd, needle);
    if (value == NULL) {
        *len = 0;
        return NULL;
    }
    value += strlen(needle);
    const char *end = strchr(value, '"');
    if (end == NULL) {
        *len = 0;
        return NULL;
    }
    *len = (size_t)(end - value);
    return value;
}

static void reply_raw(const char *event_json)
{
    post_event_json(event_json);
}

static void handle_hello(int id)
{
    char event[REMAPAD_EVENT_MAX];
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    /* 版本号单一来源：构建时写进镜像应用描述符的 PROJECT_VER（git describe），
     * 与串口 CLI version、OTA 应答、系统页信息行完全一致。 */
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(event, sizeof(event),
             "{\"t\":\"ready\",\"id\":%d,\"chip\":\"%s\",\"firmwareVersion\":\"%s\","
             "\"psramSize\":%u}",
             id, REMAPAD_CHIP_NAME, desc != NULL ? desc->version : "unknown", (unsigned)psram);
    reply_raw(event);
    ESP_LOGI(TAG, "hello -> ready (psram=%u)", (unsigned)psram);
}

/** 从 BLE 会话推导 UI 六态配对模型（ui/src/bridge/protocol.ts PairingState）。
 * 连接中的状态以协议证据为准：仅白名单放行进入握手等待的主机视为 pairing
 * 进行中，主机初始化/0x15 握手完成（或凭证匹配回连）才算 connected；
 * 手机/PC 等被立即断开的连接不改变状态。没有链路时先看广播：配对流程是
 * scanning、连接窗口是 advertising，两者都没有才落到 paired / idle（静默）。 */
static const char *real_pairing_state(void)
{
    if (ns2_session_host_registered()) {
        return "connected";
    }
    if (ns2_session_waiting_pair()) {
        return "pairing";
    }
    if (ns2_session_pairing_mode_active()) {
        return "scanning";
    }
    if (ns2_session_advertising()) {
        return "advertising";
    }
    return ns2_session_paired() ? "paired" : "idle";
}

static void handle_get_system_status(int id)
{
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"systemStatus\",\"id\":%d,\"battery\":{\"voltageMv\":%d,"
             "\"percentage\":%d,\"charging\":%s},\"backlight\":%u,\"screenOn\":%s,"
             "\"mode\":\"ble\","
             "\"pairing\":\"%s\",\"controller\":%s,\"usbRole\":\"%s\",\"usbRoleActive\":%s,"
             "\"playerLed\":%u,"
             "\"padUiMode\":%s,"
             "\"pcLink\":%s,"
             "\"uptimeMs\":%lld,"
             "\"heapFree\":%u,\"heapSize\":%u,\"psramFree\":%u}",
             id, (unsigned)battery_get_voltage_mv(), (unsigned)battery_get_percentage(),
             battery_is_charging() ? "true" : "false",
             backlight_get(), app_config_get()->screen_on ? "true" : "false",
             real_pairing_state(),
             (ns2_session_waiting_pair() || ns2_session_host_registered())
                 ? "\"pro-controller-2\"" : "null",
             s_bridge.usb_role_host ? "host" : "device",
             "true", /* 两个角色都已接入数据面：生效标志恒为真。 */
             (unsigned)ns2_session_player_leds(),
             dp_ui_active() ? "true" : "false",
             input_link_active() && input_link_pc_connected() ? "true" : "false",
             (long long)(esp_timer_get_time() / 1000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    reply_raw(event);
}

/** 设置背光并持久化（UI 命令与串口 CLI 共用）。 */
void js_bridge_set_brightness(int brightness)
{
    if (brightness < 0) {
        brightness = 0;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    const esp_err_t err = backlight_set((uint8_t)brightness);
    if (err == ESP_OK && brightness > 0) {
        /* 亮屏操作隐含恢复息屏状态；息屏走 js_bridge_screen_power。 */
        app_config_set_brightness((uint8_t)brightness);
        if (!app_config_get()->screen_on) {
            app_config_set_screen_on(true);
        }
    }
    ESP_LOGI(TAG, "backlight -> %u%%", backlight_get());
}

static void handle_set_backlight(int id, const char *cmd)
{
    const int brightness = cmd_number(cmd, "brightness");
    if (brightness < 0) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"brightness must be a number\"}",
                 id);
        reply_raw(event);
        return;
    }
    js_bridge_set_brightness(brightness);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"backlightSet\",\"id\":%d,\"brightness\":%u,\"success\":true}",
             id, backlight_get());
    reply_raw(event);
}

/** 外部任务提交命令 JSON：拷入队列，owner task 每帧取出走同一分发路径。 */
esp_err_t js_bridge_submit_command(const char *cmd_json)
{
    if (cmd_json == NULL || s_bridge.ext_cmds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ext_msg_slot_t slot;
    const size_t len = strlen(cmd_json);
    if (len >= sizeof(slot.data)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(slot.data, cmd_json);
    if (xQueueSend(s_bridge.ext_cmds, &slot, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void js_bridge_connect_key(void)
{
    /* 屏幕按钮按同一规则在 UI 侧推导，两条入口走同一对命令。 */
    const bool active = ble_controller_connected() || ns2_session_advertising();
    js_bridge_submit_command(active ? "{\"t\":\"disconnect\",\"id\":0}"
                                    : "{\"t\":\"connect\",\"id\":0}");
}

/** 外部任务向 UI 广播事件 JSON：同样经队列在 owner task 上回发。 */
void js_bridge_post_event(const char *event_json)
{
    if (event_json == NULL || s_bridge.ext_events == NULL) {
        return;
    }
    ext_msg_slot_t slot;
    if (strlen(event_json) >= sizeof(slot.data)) {
        ESP_LOGW(TAG, "ext event too long, dropped");
        return;
    }
    strcpy(slot.data, event_json);
    xQueueSend(s_bridge.ext_events, &slot, 0);
}

/** 息屏 / 亮屏（PWR 键与串口 CLI 共用）：息屏背光归零，亮屏恢复持久化
 *  亮度；状态落盘并向 UI 广播。可在任意任务上下文调用（事件经队列转移
 *  到 owner task 回发）。 */
void js_bridge_screen_power(bool on)
{
    const app_config_t *cfg = app_config_get();
    if (on == cfg->screen_on) {
        return;
    }
    app_config_set_screen_on(on);
    const uint8_t target = on ? (cfg->brightness > 0 ? cfg->brightness : 40u) : 0u;
    const esp_err_t err = backlight_set(target);
    char event[REMAPAD_EXT_MSG_LEN];
    snprintf(event, sizeof(event), "{\"t\":\"screenPowerChanged\",\"on\":%s}",
             on ? "true" : "false");
    js_bridge_post_event(event);
    ESP_LOGI(TAG, "screen power -> %s (backlight %u%%, %s)", on ? "on" : "off",
             (unsigned)target, esp_err_to_name(err));
}

static void handle_set_screen_power(int id, const char *cmd)
{
    const bool on = strstr(cmd, "\"on\":true") != NULL;
    js_bridge_screen_power(on);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"screenPowerSet\",\"id\":%d,\"on\":%s}",
             id, app_config_get()->screen_on ? "true" : "false");
    reply_raw(event);
}

/**
 * USB 角色（模式页两张卡）：device = 端口给 PC 串口（桥接帧 / 烧录 / 日志），
 * host = 端口给 OTG host 直插手柄。角色只对本次运行生效、不落盘。
 */
static void handle_set_usb_role(int id, const char *cmd)
{
    size_t role_len = 0;
    const char *role = cmd_string(cmd, "role", &role_len);
    const bool want_host = role != NULL && role_len == 4 && strncmp(role, "host", 4) == 0;
    const bool want_device = role != NULL && role_len == 6 && strncmp(role, "device", 6) == 0;
    if (!want_host && !want_device) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"unknown usb role\"}",
                 id);
        reply_raw(event);
        return;
    }
    /* 角色切换会动 USB PHY：切到 host 后 PC 上的 COM 口消失，因此先把结论
     * 发出去再切；切回串口时固件显式把 PHY 交还给 USB-Serial/JTAG。 */
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"usbRoleSet\",\"id\":%d,\"role\":\"%s\",\"active\":true}", id,
             want_host ? "host" : "device");
    reply_raw(event);
    const esp_err_t err = want_host ? usb_role_enter_host() : usb_role_leave_host();
    if (err != ESP_OK) {
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"USB_ROLE_FAILED\","
                 "\"message\":\"usb role switch failed\"}",
                 id);
        reply_raw(event);
        ESP_LOGE(TAG, "usb role switch failed: %s", esp_err_to_name(err));
        return;
    }
    s_bridge.usb_role_host = want_host;
    app_config_set_usb_role(want_host ? APP_CONFIG_USB_HOST : APP_CONFIG_USB_DEVICE);
    char broadcast[REMAPAD_EVENT_MAX];
    snprintf(broadcast, sizeof(broadcast),
             "{\"t\":\"usbRoleChanged\",\"role\":\"%s\",\"active\":true}",
             want_host ? "host" : "device");
    reply_raw(broadcast);
    ESP_LOGI(TAG, "usb role -> %s (runtime only, reset returns to serial)",
             want_host ? "host" : "device");
}

static void handle_start_pairing(int id)
{
    ns2_session_start_pairing_mode();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"%s\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "pair new host: discovery advertising, link dropped");
}

/** 连接键（屏幕「连接」、PWR 长按 3 秒）：开连接窗口等主机连上来；未配对
 *  身份改为进配对流程发发现广播。 */
static void handle_connect(int id)
{
    ns2_session_connect();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"%s\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "connect key -> %s", real_pairing_state());
}

/** 停止广播（屏幕「停止」/「断开」）：收掉连接窗口与配对流程，断开当前
 *  链路，设备回到静默。解除配对走显式 unpair 命令。 */
static void handle_disconnect(int id)
{
    ns2_session_disconnect();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"%s\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "link stopped -> %s (credentials untouched)", real_pairing_state());
}

static void handle_unpair(int id)
{
    ns2_session_unpair();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"unpairResult\",\"id\":%d,\"state\":\"%s\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "unpair -> %s", real_pairing_state());
}

/** 关机：电池供电下释放锁存即断电（之后的代码不会执行）；USB 供电下身
 *  下继续运行，由 js_bridge_service 的下一阶段确认并回报。 */
static void handle_power_off(int id)
{
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"powerOffAck\",\"id\":%d}", id);
    reply_raw(event);
    s_bridge.power_off_stage = 1;
    s_bridge.power_off_at_us = esp_timer_get_time() + REMAPAD_POWER_OFF_DELAY_US;
    ESP_LOGI(TAG, "power off requested; releasing power latch");
}

static void handle_reboot(int id)
{
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"rebooting\",\"id\":%d}", id);
    reply_raw(event);
    s_bridge.reboot_pending = true;
    s_bridge.reboot_at_us = esp_timer_get_time() + REMAPAD_REBOOT_DELAY_US;
    ESP_LOGI(TAG, "reboot requested; restarting in %lld ms",
             (long long)(REMAPAD_REBOOT_DELAY_US / 1000LL));
}

/** 调试页按键注入：a/home 单次 250ms；ui 是手柄操控 UI 的组合键。 */
static void handle_debug_key(int id, const char *cmd)
{
    size_t key_len = 0;
    const char *key = cmd_string(cmd, "key", &key_len);
    uint32_t mask = 0;
    uint32_t hold_ms = 250;
    if (key != NULL && key_len == 1 && key[0] == 'a') {
        /* 键名是 NS2 的 A 键，落在私有格式右侧的键位上（PS 的 ○）。 */
        mask = PAD_BTN_CIRCLE;
    } else if (key != NULL && key_len == 4 && strncmp(key, "home", 4) == 0) {
        /* 调试页 HOME 模拟的是实体手柄按 HOME：这里只注入按键，未连接时开
         * 唤醒窗口的动作由数据面按同一条 HOME 语义完成（见 dp_plane.c）。 */
        mask = PAD_BTN_HOME;
    } else if (key != NULL && key_len == 2 && strncmp(key, "ui", 2) == 0) {
        /* 手柄操控 UI 的组合键：保持时长盖过 dp_ui 的翻转阈值（300ms），
         * 面板上点一次就等于按下再松开组合键。 */
        mask = DP_UI_COMBO_MASK;
        hold_ms = 500;
    }
    if (mask == 0) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"unknown debug key\"}",
                 id);
        reply_raw(event);
        return;
    }
    dp_plane_debug_key(mask, hold_ms);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"debugKeySet\",\"id\":%d,\"key\":\"%.*s\"}",
             id, (int)key_len, key);
    reply_raw(event);
}

static void handle_get_controller_config(int id)
{
    const app_config_t *cfg = app_config_get();
    /* 手柄设置页展示的对外地址（公共伪装地址）；host 尚未同步时留空，
     * UI 显示占位符。 */
    uint8_t mac[6];
    char pro_mac[18] = "";
    if (ns2_session_identity_mac(NS2_ID_PRO, mac)) {
        ns2_mac_to_string(mac, pro_mac);
    }
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"controllerConfig\",\"id\":%d,\"config\":{"
             "\"bodyColor\":%lu,\"buttonColor\":%lu,\"accentColor\":%lu,"
             "\"gripColor\":%lu},"
             "\"addresses\":{\"pro\":\"%s\"}}",
             id, (unsigned long)cfg->body_color, (unsigned long)cfg->button_color,
             (unsigned long)cfg->accent_color, (unsigned long)cfg->grip_color, pro_mac);
    reply_raw(event);
}

/** 手柄配色配置（四段）：持久化并即时下发 BLE 会话（出厂块随下次广播/握手
 *  生效）。UI 的配色按钮把选中款式的一组颜色整体下发，缺项按 0 处理（沿用
 *  出厂默认）。 */
static void handle_set_controller_config(int id, const char *cmd)
{
    const uint32_t body = (uint32_t)cmd_number(cmd, "bodyColor") & 0xFFFFFFu;
    const uint32_t button = (uint32_t)cmd_number(cmd, "buttonColor") & 0xFFFFFFu;
    const uint32_t accent = (uint32_t)cmd_number(cmd, "accentColor") & 0xFFFFFFu;
    const uint32_t grip = (uint32_t)cmd_number(cmd, "gripColor") & 0xFFFFFFu;
    app_config_set_controller_colors(body, button, accent, grip);
    ns2_session_set_colors(body, button, accent, grip);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"controllerConfigSet\",\"id\":%d,\"success\":true,"
             "\"config\":{\"bodyColor\":%lu,\"buttonColor\":%lu,\"accentColor\":%lu,"
             "\"gripColor\":%lu}}",
             id, (unsigned long)body, (unsigned long)button, (unsigned long)accent,
             (unsigned long)grip);
    reply_raw(event);
    ESP_LOGI(TAG, "controller colors -> %06lx/%06lx/%06lx/%06lx (persisted)",
             (unsigned long)body, (unsigned long)button, (unsigned long)accent,
             (unsigned long)grip);
}

static void handle_get_ds_behavior(int id)
{
    const app_config_t *cfg = app_config_get();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"dsBehavior\",\"id\":%d,\"config\":{"
             "\"touchpadPlusMinus\":%s,\"captureKey\":%s}}",
             id, cfg->ds_touchpad_plus_minus ? "true" : "false",
             cfg->ds_capture_key ? "true" : "false");
    reply_raw(event);
}

/** DS 手柄行为（设置页两项开关）：落盘并在下一拍生效——数据面任务每拍按
 *  最新配置改写触摸板按下的键位（见 pad/ds_behavior.h）。 */
static void handle_set_ds_behavior(int id, const char *cmd)
{
    const bool touchpad_plus_minus = strstr(cmd, "\"touchpadPlusMinus\":true") != NULL;
    const bool capture_key = strstr(cmd, "\"captureKey\":true") != NULL;
    app_config_set_ds_behavior(touchpad_plus_minus, capture_key);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"dsBehaviorSet\",\"id\":%d,\"success\":true,\"config\":{"
             "\"touchpadPlusMinus\":%s,\"captureKey\":%s}}",
             id, touchpad_plus_minus ? "true" : "false", capture_key ? "true" : "false");
    reply_raw(event);
    ESP_LOGI(TAG, "ds behavior -> touchpad +/-=%u capture=%u (persisted)",
             (unsigned)touchpad_plus_minus, (unsigned)capture_key);
}

static void handle_cmd(const char *cmd)
{
    const int id = cmd_id(cmd);
    if (cmd_has(cmd, "\"t\":\"hello\"")) {
        handle_hello(id);
    } else if (cmd_has(cmd, "\"t\":\"getSystemStatus\"")) {
        handle_get_system_status(id);
    } else if (cmd_has(cmd, "\"t\":\"setBacklight\"")) {
        handle_set_backlight(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"setScreenPower\"")) {
        handle_set_screen_power(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"setUsbRole\"")) {
        handle_set_usb_role(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"getControllerConfig\"")) {
        handle_get_controller_config(id);
    } else if (cmd_has(cmd, "\"t\":\"setControllerConfig\"")) {
        handle_set_controller_config(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"getDsBehavior\"")) {
        handle_get_ds_behavior(id);
    } else if (cmd_has(cmd, "\"t\":\"setDsBehavior\"")) {
        handle_set_ds_behavior(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"startPairing\"")) {
        handle_start_pairing(id);
    } else if (cmd_has(cmd, "\"t\":\"connect\"")) {
        handle_connect(id);
    } else if (cmd_has(cmd, "\"t\":\"disconnect\"")) {
        handle_disconnect(id);
    } else if (cmd_has(cmd, "\"t\":\"unpair\"")) {
        handle_unpair(id);
    } else if (cmd_has(cmd, "\"t\":\"debugKey\"")) {
        handle_debug_key(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"powerOff\"")) {
        handle_power_off(id);
    } else if (cmd_has(cmd, "\"t\":\"reboot\"")) {
        handle_reboot(id);
    } else {
        ESP_LOGW(TAG, "unsupported cmd: %.96s", cmd);
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"NOT_IMPLEMENTED\","
                 "\"message\":\"command needs the data plane\"}",
                 id);
        reply_raw(event);
    }
}

/** 每帧轮询 BLE 会话状态，发现变化即广播 pairingStateChanged 事件。 */
static void pairing_state_poll(void)
{
    const char *state = real_pairing_state();
    if (s_bridge.last_pairing_state == NULL ||
        strcmp(s_bridge.last_pairing_state, state) != 0) {
        s_bridge.last_pairing_state = state;
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"pairingStateChanged\",\"state\":\"%s\"}", state);
        reply_raw(event);
        ESP_LOGI(TAG, "pairing state -> %s", state);
    }
}

const char *js_bridge_pairing_state(void)
{
    return real_pairing_state();
}

/** 每帧轮询手柄操控模式：变化即广播 padUiModeChanged，屏幕据此显示提示条
 *  （组合键由数据面判定，面板只负责转达结论）。 */
static void pad_ui_mode_poll(void)
{
    const bool on = dp_ui_active();
    if (s_bridge.last_pad_ui_mode == on) {
        return;
    }
    s_bridge.last_pad_ui_mode = on;
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"padUiModeChanged\",\"on\":%s}",
             on ? "true" : "false");
    reply_raw(event);
    ESP_LOGI(TAG, "pad ui mode -> %s", on ? "on" : "off");
}

/** 每帧轮询主机下发的玩家序号灯掩码（Command 0x09）：变化即广播
 *  playerLedChanged，首页四格指示灯据此更新；断开连接后掩码回落到 0。 */
static void player_led_poll(void)
{
    const int led = (int)ns2_session_player_leds();
    if (s_bridge.last_player_led == led) {
        return;
    }
    s_bridge.last_player_led = led;
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"playerLedChanged\",\"led\":%d}", led);
    reply_raw(event);
    ESP_LOGI(TAG, "player led -> 0x%x", (unsigned)led);
}

/** 每帧轮询 USB 直插手柄的接入状态：变化即广播 padAttachedChanged，
 *  底栏左区在「手柄」档下据此把图标切成手柄或它的禁用形态。
 *  name 是家族短名（机读 token，屏幕文案由 UI 侧的字面量给出）。 */
static void pad_attached_poll(void)
{
    const bool attached = usb_input_attached();
    if (s_bridge.last_pad_attached == (int)attached) {
        return;
    }
    s_bridge.last_pad_attached = (int)attached;
    uint16_t vid = 0;
    uint16_t pid = 0;
    const char *name = "unknown";
    if (attached && usb_input_device_ids(&vid, &pid, NULL)) {
        name = pad_family_name(pad_family_from_ids(vid, pid));
    }
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"padAttachedChanged\",\"attached\":%s,\"name\":\"%s\"}",
             attached ? "true" : "false", name);
    reply_raw(event);
    ESP_LOGI(TAG, "pad attached -> %s (%s)", attached ? "yes" : "no", name);
}

/** 每帧轮询串口上的 PC（判据见 input_link_pc_connected：USB-Serial/JTAG 在收
 *  主机的 SOF，插充电宝不算），端口交给 OTG host 时串口不在手上、恒报未接入。
 *  变化即广播 pcLinkChanged，底栏左区在「串口」档下据此切成电脑图标或禁用形态。 */
static void pc_link_poll(void)
{
    const bool connected = input_link_active() && input_link_pc_connected();
    if (s_bridge.last_pc_connected == (int)connected) {
        return;
    }
    s_bridge.last_pc_connected = (int)connected;
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"pcLinkChanged\",\"connected\":%s}",
             connected ? "true" : "false");
    reply_raw(event);
    ESP_LOGI(TAG, "pc link -> %s", connected ? "up" : "down");
}

void js_bridge_service(void)
{
    pairing_state_poll();
    player_led_poll();
    pad_ui_mode_poll();
    pad_attached_poll();
    pc_link_poll();

    /* 关机两阶段：先释放电源锁存，再确认是否真的断电。电池供电时第一步
     * 之后系统已经断电、不回到这里；能走到第二步说明外部供电旁路了锁存。 */
    if (s_bridge.power_off_stage == 1 && esp_timer_get_time() >= s_bridge.power_off_at_us) {
        pwr_key_power_release();
        s_bridge.power_off_stage = 2;
        s_bridge.power_off_at_us = esp_timer_get_time() + REMAPAD_POWER_OFF_VERIFY_US;
    } else if (s_bridge.power_off_stage == 2 &&
               esp_timer_get_time() >= s_bridge.power_off_at_us) {
        s_bridge.power_off_stage = 0;
        pwr_key_power_hold();
        reply_raw("{\"t\":\"powerOffBlocked\"}");
        ESP_LOGI(TAG, "power off blocked: still powered, latch restored (USB supply?)");
    }

    if (s_bridge.reboot_pending && esp_timer_get_time() >= s_bridge.reboot_at_us) {
        ESP_LOGI(TAG, "rebooting now (USB returns to Serial/JTAG COM mode)");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    /* 外部任务（PWR / 串口 CLI）的命令与事件在 owner task 上出队处理。 */
    ext_msg_slot_t ext;
    while (xQueueReceive(s_bridge.ext_events, &ext, 0) == pdTRUE) {
        reply_raw(ext.data);
    }
    while (xQueueReceive(s_bridge.ext_cmds, &ext, 0) == pdTRUE) {
        handle_cmd(ext.data);
    }

    while (s_bridge.queue_len > 0) {
        bridge_cmd_slot_t slot = s_bridge.queue[s_bridge.queue_head];
        s_bridge.queue_head = (s_bridge.queue_head + 1) % REMAPAD_BRIDGE_QUEUE_LEN;
        s_bridge.queue_len--;
        handle_cmd(slot.data);
    }

    /* 命令先结算再动 BLE 栈：刚开出的窗口不会被省电服务立刻收掉。 */
    ns2_session_ble_service();
}

/** 控制面服务任务参数：与界面任务同核（CPU1），优先级低于界面渲染。 */
#define REMAPAD_BRIDGE_TASK_NAME "remapad-bridge"
/** 栈给到 8 KB：BLE 栈的起停（nimble_port_init / stop）就在这条任务上跑。 */
#define REMAPAD_BRIDGE_TASK_STACK_BYTES (8U * 1024U)
#define REMAPAD_BRIDGE_TASK_PRIORITY 4
#define REMAPAD_BRIDGE_TASK_CORE 1
#define REMAPAD_BRIDGE_SERVICE_PERIOD_MS 50U

/** 控制面泵：周期驱动命令队列与配对状态机（无 UI 构建也靠它活着）。 */
static void bridge_service_task(void *opaque)
{
    (void)opaque;
    for (;;) {
        js_bridge_service();
        vTaskDelay(pdMS_TO_TICKS(REMAPAD_BRIDGE_SERVICE_PERIOD_MS));
    }
}

esp_err_t js_bridge_service_start(void)
{
    if (xTaskCreatePinnedToCore(bridge_service_task, REMAPAD_BRIDGE_TASK_NAME,
                                REMAPAD_BRIDGE_TASK_STACK_BYTES / sizeof(StackType_t), NULL,
                                REMAPAD_BRIDGE_TASK_PRIORITY, NULL,
                                REMAPAD_BRIDGE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "bridge service task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

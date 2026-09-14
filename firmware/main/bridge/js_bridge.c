#include "js_bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
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
#include "ns2_identity.h"
#include "pad_state.h"
#include "pwr_key.h"

#include "pocketjs/guest.h"

static const char *TAG = "remapad_bridge";

#define REMAPAD_FW_VERSION "v0.4.0"
#define REMAPAD_CHIP_NAME "ESP32-S3"
#define REMAPAD_BRIDGE_CMD_MAX 256
#define REMAPAD_BRIDGE_QUEUE_LEN 8
#define REMAPAD_EVENT_MAX 320

/** 重启前留出的应答时间：先让 UI 收到 rebooting 再重启。 */
#define REMAPAD_REBOOT_DELAY_US (150 * 1000LL)

/** 关机时序：先让 UI 收到应答，再释放电源锁存；释放后仍存活说明锁存被
 *  外部供电旁路（USB 供电），此时恢复锁存并如实回报 UI。 */
#define REMAPAD_POWER_OFF_DELAY_US (200 * 1000LL)
#define REMAPAD_POWER_OFF_VERIFY_US (1500 * 1000LL)

/** 外部命令/事件槽：PWR 按键与串口 CLI 等非 owner task 上下文的入口。
 * guest eval 只允许在 owner task 上执行（QuickJS 栈守卫约束），外部任务
 * 只把字符串拷进队列，js_bridge_service 每帧在 owner task 上取出处理。 */
#define REMAPAD_EXT_MSG_LEN 160
#define REMAPAD_EXT_QUEUE_LEN 4

typedef struct {
    char data[REMAPAD_EXT_MSG_LEN];
} ext_msg_slot_t;

typedef struct {
    char data[REMAPAD_BRIDGE_CMD_MAX];
} bridge_cmd_slot_t;

static struct {
    pocketjs_guest_t *guest;
    bridge_cmd_slot_t queue[REMAPAD_BRIDGE_QUEUE_LEN];
    size_t queue_head;
    size_t queue_len;
    QueueHandle_t ext_cmds;
    QueueHandle_t ext_events;
    const char *last_pairing_state; /* 字面量常量指针，用于变化检测。 */
    bool usb_role_host; /* UI 请求的角色；host 数据面未接入，仅记录。 */
    int64_t reboot_at_us;
    bool reboot_pending;
    /** 关机阶段：0 空闲，1 待释放锁存，2 待确认是否已断电。 */
    int64_t power_off_at_us;
    uint8_t power_off_stage;
} s_bridge;

esp_err_t js_bridge_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.ext_cmds = xQueueCreate(REMAPAD_EXT_QUEUE_LEN, sizeof(ext_msg_slot_t));
    s_bridge.ext_events = xQueueCreate(REMAPAD_EXT_QUEUE_LEN, sizeof(ext_msg_slot_t));
    if (s_bridge.ext_cmds == NULL || s_bridge.ext_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* USB 角色与手柄身份从持久化配置恢复（桥接角色永不落盘）。 */
    s_bridge.usb_role_host = app_config_get()->usb_role == APP_CONFIG_USB_HOST;
    ns2_session_set_identity(app_config_get()->ctrl_type == APP_CONFIG_CTRL_JOYCON,
                             app_config_get()->body_color,
                             app_config_get()->button_color,
                             app_config_get()->grip_color);
    return ESP_OK;
}

void js_bridge_attach(pocketjs_guest_t *guest)
{
    s_bridge.guest = guest;
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

/** 通过 eval 调用 guest 的 __onNativeBridgeMessage；JSON 必须是合法 JS 表达式。 */
static void post_event_json(const char *event_json)
{
    if (s_bridge.guest == NULL) {
        ESP_LOGI(TAG, "event (no guest): %s", event_json);
        return;
    }
    char eval_buf[REMAPAD_EVENT_MAX + 48];
    const int n = snprintf(eval_buf, sizeof(eval_buf),
                           "__onNativeBridgeMessage&&__onNativeBridgeMessage(%s)", event_json);
    if (n < 0 || (size_t)n >= sizeof(eval_buf)) {
        ESP_LOGW(TAG, "event too long, dropped");
        return;
    }
    const esp_err_t err = pocketjs_guest_eval(s_bridge.guest, eval_buf, (size_t)n, "bridge_evt");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event eval failed: %s", esp_err_to_name(err));
    }
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
    snprintf(event, sizeof(event),
             "{\"t\":\"ready\",\"id\":%d,\"chip\":\"%s\",\"firmwareVersion\":\"%s\","
             "\"psramSize\":%u}",
             id, REMAPAD_CHIP_NAME, REMAPAD_FW_VERSION, (unsigned)psram);
    reply_raw(event);
    ESP_LOGI(TAG, "hello -> ready (psram=%u)", (unsigned)psram);
}

/** 从 BLE 会话推导 UI 六态配对模型（ui/src/bridge/protocol.ts PairingState）。
 * 连接中的状态以协议证据为准：仅白名单放行进入握手等待的主机视为 pairing
 * 进行中，主机初始化/0x15 握手完成（或凭证匹配回连）才算 connected；
 * 手机/PC 等被立即断开的连接不改变状态。 */
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
             "\"uptimeMs\":%lld,"
             "\"heapFree\":%u,\"heapSize\":%u,\"psramFree\":%u}",
             id, (unsigned)battery_get_voltage_mv(), (unsigned)battery_get_percentage(),
             battery_is_charging() ? "true" : "false",
             backlight_get(), app_config_get()->screen_on ? "true" : "false",
             real_pairing_state(),
             (ns2_session_waiting_pair() || ns2_session_host_registered())
                 ? "\"pro-controller-2\"" : "null",
             s_bridge.usb_role_host ? "host" : "device",
             s_bridge.usb_role_host ? "false" : "true",
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

static void handle_set_usb_role(int id, const char *cmd)
{
    size_t role_len = 0;
    const char *role = cmd_string(cmd, "role", &role_len);
    const bool want_host = role != NULL && role_len == 4 && strncmp(role, "host", 4) == 0;
    const bool want_otg = role != NULL && role_len == 3 && strncmp(role, "otg", 3) == 0;
    const bool known = want_host || want_otg ||
                       (role != NULL && role_len == 6 && strncmp(role, "device", 6) == 0);
    if (!known) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"unknown usb role\"}",
                 id);
        reply_raw(event);
        return;
    }
    /* 桥接（otg）开发期临时禁用防误操作：USB PHY 切换会断开 COM（无人
     * 值守时无法烧录），数据面也未接入。UI 已移除该选项，这里静默跳过：
     * 不应用、不报错，回复当前角色，待 M5 数据面接入后恢复。 */
    if (want_otg) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"usbRoleSet\",\"id\":%d,\"role\":\"%s\",\"active\":%s}",
                 id, s_bridge.usb_role_host ? "host" : "device",
                 s_bridge.usb_role_host ? "false" : "true");
        reply_raw(event);
        ESP_LOGI(TAG, "usb role otg skipped (dev-time lock, no error surfaced)");
        return;
    }
    s_bridge.usb_role_host = want_host;
    app_config_set_usb_role(want_host ? APP_CONFIG_USB_HOST : APP_CONFIG_USB_DEVICE);
    /* USB PHY/OTG 切换属于数据面，尚未接入：这里只记录本次运行的角色（不
     * 落盘，重启回到串口），如实上报，不触碰 RTC_CNTL USB mux。接入后按
     * docs/hardware.md 的机制实现。 */
    char event[REMAPAD_EVENT_MAX];
    if (want_host) {
        snprintf(event, sizeof(event),
                 "{\"t\":\"usbRoleSet\",\"id\":%d,\"role\":\"host\",\"active\":false,"
                 "\"message\":\"USB host 数据面未接入，切换暂不生效\"}",
                 id);
    } else {
        snprintf(event, sizeof(event),
                 "{\"t\":\"usbRoleSet\",\"id\":%d,\"role\":\"device\",\"active\":true}",
                 id);
    }
    reply_raw(event);

    char broadcast[REMAPAD_EVENT_MAX];
    snprintf(broadcast, sizeof(broadcast),
             "{\"t\":\"usbRoleChanged\",\"role\":\"%s\",\"active\":%s}",
             want_host ? "host" : "device", want_host ? "false" : "true");
    reply_raw(broadcast);
    ESP_LOGI(TAG, "usb role request -> %s (runtime only, phy untouched)",
             want_host ? "host" : "device");
}

static void handle_start_pairing(int id)
{
    ns2_session_start_pairing_mode();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"scanning\","
             "\"message\":\"广播中，等待主机连接\"}",
             id);
    reply_raw(event);
    ESP_LOGI(TAG, "pairing mode on (real BLE advertising)");
}

static void handle_stop_pairing(int id)
{
    /* 停止搜索退出配对模式并恢复常规广播；连接中但注册握手未完成的主机
     * 一并断开，否则 pairing 状态由连接驱动、停止永远无法退出。凭证以
     * 协议证据为准持久化，解除配对走显式 unpair 命令。 */
    ns2_session_stop_pairing_mode();
    if (!ns2_session_host_registered()) {
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
    }
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"%s\","
             "\"message\":\"已退出配对模式\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "pairing mode stopped (credentials untouched)");
}

static void handle_unpair(int id)
{
    ns2_session_unpair();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"unpairResult\",\"id\":%d,\"state\":\"%s\","
             "\"message\":\"已解除配对\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "unpair -> %s", real_pairing_state());
}

/** 配对页「按下 LR」：Pro 走调试注入（部分注册界面用它确认）；JoyCon 组合
 * 交给会话层——确保左右双广播在发并注入 L+R（组合确认动作）。 */
static void handle_press_lr(int id)
{
    if (app_config_get()->ctrl_type == APP_CONFIG_CTRL_JOYCON) {
        ns2_session_press_lr();
    } else {
        dp_plane_debug_key(NS2_BTN_L | NS2_BTN_R, 1000);
    }
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event), "{\"t\":\"pressLrAck\",\"id\":%d,\"success\":true}", id);
    reply_raw(event);
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

/** 调试页按键注入：a/home 单次 250ms；lr 同时按下 L 和 R 保持 1s，
 *  对应主机 Grip/顺序界面的配对确认动作。 */
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
        mask = PAD_BTN_GUIDE;
    } else if (key != NULL && key_len == 2 && strncmp(key, "lr", 2) == 0) {
        mask = PAD_BTN_LB | PAD_BTN_RB;
        hold_ms = 1000;
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
    /* 手柄设置页展示的对外地址：Pro 公共伪装地址，JoyCon 各自派生地址。
     * host 尚未同步时留空，UI 显示占位符。 */
    uint8_t mac[6];
    char pro_mac[18] = "";
    char left_mac[18] = "";
    char right_mac[18] = "";
    if (ns2_session_identity_mac(NS2_ID_PRO, mac)) {
        ns2_mac_to_string(mac, pro_mac);
    }
    if (ns2_session_identity_mac(NS2_ID_JOYCON_L, mac)) {
        ns2_mac_to_string(mac, left_mac);
    }
    if (ns2_session_identity_mac(NS2_ID_JOYCON_R, mac)) {
        ns2_mac_to_string(mac, right_mac);
    }
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"controllerConfig\",\"id\":%d,\"config\":{\"type\":\"%s\","
             "\"bodyColor\":%lu,\"buttonColor\":%lu,\"gripColor\":%lu},"
             "\"addresses\":{\"pro\":\"%s\",\"left\":\"%s\",\"right\":\"%s\"}}",
             id, cfg->ctrl_type == APP_CONFIG_CTRL_JOYCON ? "joycon" : "pro",
             (unsigned long)cfg->body_color, (unsigned long)cfg->button_color,
             (unsigned long)cfg->grip_color, pro_mac, left_mac, right_mac);
    reply_raw(event);
}

/** 手柄身份配置：类型 + 配色持久化并即时下发 BLE 会话（出厂块随下次
 *  广播/握手生效）。颜色选择 UI 预留，字段先全链路贯通。 */
static void handle_set_controller_config(int id, const char *cmd)
{
    size_t type_len = 0;
    const char *type = cmd_string(cmd, "type", &type_len);
    const bool joycon = type != NULL && type_len == 6 && strncmp(type, "joycon", 6) == 0;
    const bool pro = type != NULL && type_len == 3 && strncmp(type, "pro", 3) == 0;
    if (!joycon && !pro) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"unknown controller type\"}",
                 id);
        reply_raw(event);
        return;
    }
    const uint32_t body = (uint32_t)cmd_number(cmd, "bodyColor") & 0xFFFFFFu;
    const uint32_t button = (uint32_t)cmd_number(cmd, "buttonColor") & 0xFFFFFFu;
    const uint32_t grip = (uint32_t)cmd_number(cmd, "gripColor") & 0xFFFFFFu;
    app_config_set_controller(joycon ? APP_CONFIG_CTRL_JOYCON : APP_CONFIG_CTRL_PRO,
                              body, button, grip);
    ns2_session_set_identity(joycon, body, button, grip);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"controllerConfigSet\",\"id\":%d,\"success\":true,"
             "\"config\":{\"type\":\"%s\",\"bodyColor\":%lu,\"buttonColor\":%lu,"
             "\"gripColor\":%lu}}",
             id, joycon ? "joycon" : "pro", (unsigned long)body,
             (unsigned long)button, (unsigned long)grip);
    reply_raw(event);
    ESP_LOGI(TAG, "controller config -> %s (persisted)", joycon ? "joycon" : "pro");
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
    } else if (cmd_has(cmd, "\"t\":\"startPairing\"")) {
        handle_start_pairing(id);
    } else if (cmd_has(cmd, "\"t\":\"stopPairing\"")) {
        handle_stop_pairing(id);
    } else if (cmd_has(cmd, "\"t\":\"unpair\"")) {
        handle_unpair(id);
    } else if (cmd_has(cmd, "\"t\":\"pressLr\"")) {
        handle_press_lr(id);
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

void js_bridge_service(void)
{
    pairing_state_poll();

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
}

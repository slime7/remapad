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
#include "ble_controller.h"
#include "ble_session.h"

#include "pocketjs/guest.h"

static const char *TAG = "remapad_bridge";

#define REMAPAD_FW_VERSION "v0.3.0"
#define REMAPAD_CHIP_NAME "ESP32-S3"
#define REMAPAD_BRIDGE_CMD_MAX 256
#define REMAPAD_BRIDGE_QUEUE_LEN 8
#define REMAPAD_EVENT_MAX 320

/** battery.c 仍是预留占位；在真实 ADC 驱动接入前上报固定值。 */
#define REMAPAD_BATTERY_MV 4120
#define REMAPAD_BATTERY_PCT 88

#define REMAPAD_REBOOT_DELAY_US (150 * 1000LL)

typedef struct {
    char data[REMAPAD_BRIDGE_CMD_MAX];
} bridge_cmd_slot_t;

static struct {
    pocketjs_guest_t *guest;
    bridge_cmd_slot_t queue[REMAPAD_BRIDGE_QUEUE_LEN];
    size_t queue_head;
    size_t queue_len;
    const char *last_pairing_state; /* 字面量常量指针，用于变化检测。 */
    bool usb_role_host; /* UI 请求的角色；host 数据面未接入，仅记录。 */
    int64_t reboot_at_us;
    bool reboot_pending;
} s_bridge;

esp_err_t js_bridge_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));
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

/** 从 BLE 会话推导 UI 六态配对模型（ui/src/bridge/protocol.ts PairingState）。 */
static const char *real_pairing_state(void)
{
    if (ble_controller_connected()) {
        return ns2_session_pairing_mode_active() ? "pairing" : "connected";
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
             "\"percentage\":%d,\"charging\":false},\"backlight\":%u,\"mode\":\"ble\","
             "\"pairing\":\"%s\",\"controller\":%s,\"usbRole\":\"%s\",\"usbRoleActive\":%s,"
             "\"uptimeMs\":%lld,"
             "\"heapFree\":%u,\"heapSize\":%u,\"psramFree\":%u}",
             id, REMAPAD_BATTERY_MV, REMAPAD_BATTERY_PCT, backlight_get(),
             real_pairing_state(),
             ble_controller_connected() ? "\"pro-controller-2\"" : "null",
             s_bridge.usb_role_host ? "host" : "device",
             s_bridge.usb_role_host ? "false" : "true",
             (long long)(esp_timer_get_time() / 1000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    reply_raw(event);
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
    const esp_err_t err = backlight_set((uint8_t)brightness);
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"backlightSet\",\"id\":%d,\"brightness\":%u,\"success\":%s}",
             id, backlight_get(), err == ESP_OK ? "true" : "false");
    reply_raw(event);
    ESP_LOGI(TAG, "backlight -> %u%%", backlight_get());
}

static void handle_set_usb_role(int id, const char *cmd)
{
    size_t role_len = 0;
    const char *role = cmd_string(cmd, "role", &role_len);
    const bool want_host = role != NULL && role_len == 4 && strncmp(role, "host", 4) == 0;
    const bool known = want_host || (role != NULL && role_len == 6 && strncmp(role, "device", 6) == 0);
    if (!known) {
        char event[REMAPAD_EVENT_MAX];
        snprintf(event, sizeof(event),
                 "{\"t\":\"error\",\"id\":%d,\"code\":\"BAD_REQUEST\","
                 "\"message\":\"unknown usb role\"}",
                 id);
        reply_raw(event);
        return;
    }
    s_bridge.usb_role_host = want_host;
    /* USB PHY/OTG 切换属于数据面，尚未接入：这里只记录请求并如实上报，
     * 不触碰 RTC_CNTL USB mux。接入后按 docs/hardware.md 的机制实现。 */
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
    ESP_LOGI(TAG, "usb role request -> %s (phy untouched)", want_host ? "host" : "device");
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
    ns2_session_stop_pairing_mode();
    char event[REMAPAD_EVENT_MAX];
    snprintf(event, sizeof(event),
             "{\"t\":\"pairingResult\",\"id\":%d,\"state\":\"%s\","
             "\"message\":\"已停止配对\"}",
             id, real_pairing_state());
    reply_raw(event);
    ESP_LOGI(TAG, "pairing mode off");
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

static void handle_cmd(const char *cmd)
{
    const int id = cmd_id(cmd);
    if (cmd_has(cmd, "\"t\":\"hello\"")) {
        handle_hello(id);
    } else if (cmd_has(cmd, "\"t\":\"getSystemStatus\"")) {
        handle_get_system_status(id);
    } else if (cmd_has(cmd, "\"t\":\"setBacklight\"")) {
        handle_set_backlight(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"setUsbRole\"")) {
        handle_set_usb_role(id, cmd);
    } else if (cmd_has(cmd, "\"t\":\"startPairing\"")) {
        handle_start_pairing(id);
    } else if (cmd_has(cmd, "\"t\":\"stopPairing\"")) {
        handle_stop_pairing(id);
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

void js_bridge_service(void)
{
    pairing_state_poll();

    if (s_bridge.reboot_pending && esp_timer_get_time() >= s_bridge.reboot_at_us) {
        ESP_LOGI(TAG, "rebooting now (USB returns to Serial/JTAG COM mode)");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    while (s_bridge.queue_len > 0) {
        bridge_cmd_slot_t slot = s_bridge.queue[s_bridge.queue_head];
        s_bridge.queue_head = (s_bridge.queue_head + 1) % REMAPAD_BRIDGE_QUEUE_LEN;
        s_bridge.queue_len--;
        handle_cmd(slot.data);
    }
}

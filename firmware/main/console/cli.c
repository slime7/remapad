#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "backlight.h"
#include "battery.h"
#include "bridge/js_bridge.h"
#include "ble_session.h"
#include "buzzer.h"
#include "dp_source.h"
#include "ns2_identity.h"
#include "pad_state.h"

static const char *TAG = "remapad_cli";

#define CLI_LINE_MAX 96

static void cli_print(const char *text)
{
    /* 控制台输出同样经 USJ vfs（非阻塞，未连接时丢弃，绝不阻塞调用任务）。 */
    printf("%s\r\n", text);
    fflush(stdout);
}

static void cli_help(void)
{
    cli_print("remapad cli commands:");
    cli_print("  status              system status one-liner");
    cli_print("  key <name> [ms]     inject debug key (key release clears)");
    cli_print("                      a b x y plus minus home capture c l r zl zr");
    cli_print("                      ls rs up down left right gl gr lr");
    cli_print("  stick l|r <x> <y>   set stick level (0-4095, center)");
    cli_print("  stick reset         center both sticks");
    cli_print("  link                per-identity BLE link status");
    cli_print("  backlight 0-100     set + persist backlight");
    cli_print("  screen on|off       screen power");
    cli_print("  beep [ms]           buzzer hint tone (default 120)");
    cli_print("  mode device|host    usb connection mode");
    cli_print("  pairing start|stop  pairing advertising");
    cli_print("  poweroff            release power latch (battery only)");
    cli_print("  reboot              restart into COM mode");
}

static void cli_status(void)
{
    char line[128];
    const app_config_t *cfg = app_config_get();
    snprintf(line, sizeof(line),
             "state pairing=%s role=%s backlight=%u screen=%u uptime=%llds heap=%u "
             "batt=%umV/%u%% chg=%u",
             js_bridge_pairing_state(),
             cfg->usb_role == APP_CONFIG_USB_HOST ? "host" : "device",
             (unsigned)backlight_get(), (unsigned)cfg->screen_on,
             (long long)(esp_timer_get_time() / 1000000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)battery_get_voltage_mv(), (unsigned)battery_get_percentage(),
             battery_is_charging() ? 1u : 0u);
    cli_print(line);
}

/** 调试注入：按键名 + 可选保持时长；release 立即释放当前注入。 */
static void cli_key(const char *arg)
{
    if (strcmp(arg, "release") == 0) {
        dp_source_inject_release();
        cli_print("ok keys released");
        return;
    }
    char name[16] = {0};
    int hold_ms = 0;
    const int fields = sscanf(arg, "%15s %d", name, &hold_ms);
    uint32_t mask = 0;
    uint32_t default_hold_ms = 0;
    if (fields < 1 || !dp_source_key_lookup(name, strlen(name), &mask, &default_hold_ms)) {
        cli_print("err usage: key <name> [ms] | key release");
        return;
    }
    const uint32_t hold = fields >= 2 && hold_ms > 0 ? (uint32_t)hold_ms : default_hold_ms;
    dp_source_inject(mask, hold);
    cli_print("ok key injected");
}

/** 摇杆轴取值：0-4095 整数，或 center / c 表示中位。 */
static bool parse_stick_axis(const char *text, int *value)
{
    if (strcmp(text, "center") == 0 || strcmp(text, "c") == 0) {
        *value = PAD_AXIS_CENTER;
        return true;
    }
    char *end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0 || parsed > PAD_AXIS_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

/** 调试注入：设定一侧摇杆电平；reset 两侧回中。 */
static void cli_stick(const char *arg)
{
    if (strcmp(arg, "reset") == 0) {
        dp_source_inject_stick_reset();
        cli_print("ok sticks centered");
        return;
    }
    char side[8] = {0};
    char axis_x[8] = {0};
    char axis_y[8] = {0};
    int x = 0;
    int y = 0;
    if (sscanf(arg, "%7s %7s %7s", side, axis_x, axis_y) < 3 ||
        (strcmp(side, "l") != 0 && strcmp(side, "r") != 0) ||
        !parse_stick_axis(axis_x, &x) || !parse_stick_axis(axis_y, &y)) {
        cli_print("err usage: stick l|r <x> <y> (0-4095 or center) | stick reset");
        return;
    }
    dp_source_inject_stick(side[0], (uint16_t)x, (uint16_t)y);
    cli_print("ok stick set");
}

static const char *link_state_name(uint8_t state)
{
    switch (state) {
    case NS2_LINK_ADVERTISING:
        return "advertising";
    case NS2_LINK_WAIT_PAIR:
        return "wait-pair";
    case NS2_LINK_NORMAL:
        return "normal";
    default:
        return "idle";
    }
}

/** BLE 链路观测：当前形态每个身份一行，含对外地址与上报计数。 */
static void cli_link(void)
{
    uint8_t ids[2] = {0};
    const size_t count = ns2_session_mode_identities(ids);
    char line[128];
    snprintf(line, sizeof(line), "mode=%s identities=%u",
             count == 1 ? "pro" : "joycon", (unsigned)count);
    cli_print(line);
    for (size_t i = 0; i < count; i++) {
        ns2_session_status_t status;
        if (!ns2_session_status(ids[i], &status)) {
            continue;
        }
        char addr[18] = "--";
        if (status.mac_valid) {
            ns2_mac_to_string(status.mac, addr);
        }
        if (status.connected) {
            snprintf(line, sizeof(line),
                     "  %-4s %s conn=%u fmt=0x%02x notify=%s%s reports=%lu creds=%u "
                     "adv=%s addr=%s",
                     ns2_identity_name(status.identity), link_state_name(status.state),
                     status.conn_handle, status.report_format,
                     status.notify_05 ? "05" : "-", status.notify_09 ? "09" : "-",
                     (unsigned long)status.reports, (unsigned)status.creds,
                     status.advertising ? "on" : "off", addr);
        } else {
            snprintf(line, sizeof(line), "  %-4s %s creds=%u adv=%s addr=%s",
                     ns2_identity_name(status.identity), link_state_name(status.state),
                     (unsigned)status.creds, status.advertising ? "on" : "off", addr);
        }
        cli_print(line);
    }
}

static void cli_backlight(const char *arg)
{
    const int value = atoi(arg);
    if (value < 0 || value > 100) {
        cli_print("err backlight 0-100");
        return;
    }
    js_bridge_set_brightness(value);
    cli_print("ok backlight applied");
}

static void cli_screen(const char *arg)
{
    if (strcmp(arg, "on") == 0) {
        js_bridge_screen_power(true);
        cli_print("ok screen on");
    } else if (strcmp(arg, "off") == 0) {
        js_bridge_screen_power(false);
        cli_print("ok screen off");
    } else {
        cli_print("err usage: screen on|off");
    }
}

/** 蜂鸣器自检：不依赖 PWR 按键，便于确认提示音通路与背光互不影响。 */
static void cli_beep(const char *arg)
{
    const int value = arg[0] == '\0' ? 120 : atoi(arg);
    if (value <= 0 || value > 1000) {
        cli_print("err beep 1-1000 ms");
        return;
    }
    buzzer_beep((uint32_t)value);
    cli_print("ok beep queued");
}

static void cli_mode(const char *arg)
{
    /* 走 bridge 命令路径：otg 在 bridge 内拒绝并回复；角色只对本次运行生效。 */
    if (strcmp(arg, "device") == 0 || strcmp(arg, "host") == 0) {
        char json[64];
        snprintf(json, sizeof(json), "{\"t\":\"setUsbRole\",\"role\":\"%s\",\"id\":0}", arg);
        js_bridge_submit_command(json);
        cli_print("ok mode request queued");
    } else if (strcmp(arg, "otg") == 0) {
        cli_print("err bridge mode is locked");
    } else {
        cli_print("err usage: mode device|host");
    }
}

static void cli_pairing(const char *arg)
{
    if (strcmp(arg, "start") == 0) {
        js_bridge_submit_command("{\"t\":\"startPairing\",\"id\":0}");
        cli_print("ok pairing start queued");
    } else if (strcmp(arg, "stop") == 0) {
        js_bridge_submit_command("{\"t\":\"stopPairing\",\"id\":0}");
        cli_print("ok pairing stop queued");
    } else {
        cli_print("err usage: pairing start|stop");
    }
}

static void cli_dispatch(char *line)
{
    char *space = strchr(line, ' ');
    if (space != NULL) {
        *space = '\0';
        space++;
    }
    const char *arg = space != NULL ? space : "";

    if (strcmp(line, "help") == 0) {
        cli_help();
    } else if (strcmp(line, "ping") == 0) {
        cli_print("pong");
    } else if (strcmp(line, "status") == 0) {
        cli_status();
    } else if (strcmp(line, "key") == 0) {
        cli_key(arg);
    } else if (strcmp(line, "stick") == 0) {
        cli_stick(arg);
    } else if (strcmp(line, "link") == 0) {
        cli_link();
    } else if (strcmp(line, "backlight") == 0) {
        cli_backlight(arg);
    } else if (strcmp(line, "screen") == 0) {
        cli_screen(arg);
    } else if (strcmp(line, "beep") == 0) {
        cli_beep(arg);
    } else if (strcmp(line, "mode") == 0) {
        cli_mode(arg);
    } else if (strcmp(line, "pairing") == 0) {
        cli_pairing(arg);
    } else if (strcmp(line, "poweroff") == 0) {
        js_bridge_submit_command("{\"t\":\"powerOff\",\"id\":0}");
        cli_print("ok poweroff queued");
    } else if (strcmp(line, "reboot") == 0) {
        js_bridge_submit_command("{\"t\":\"reboot\",\"id\":0}");
        cli_print("ok reboot queued");
    } else if (line[0] != '\0') {
        cli_print("err unknown command, try help");
    }
}

void cli_feed_bytes(const uint8_t *data, size_t len)
{
    static char line[CLI_LINE_MAX];
    static size_t used;
    if (data == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        const char ch = (char)data[i];
        if (ch == '\r' || ch == '\n') {
            line[used] = '\0';
            cli_dispatch(line);
            used = 0;
            continue;
        }
        if (ch == '\0') {
            continue; /* 帧噪声或空字节：不进入命令行缓冲。 */
        }
        if (used + 1 < sizeof(line)) {
            line[used++] = ch;
        } else {
            cli_print("err line too long");
            used = 0;
        }
    }
}

esp_err_t remapad_cli_start(void)
{
    /* 接收与分帧由 input_link 的接收任务承担（USJ 驱动 + 环形缓冲），这里
     * 只报告命令行就绪；命令分发在 cli_feed_bytes 里同步进行。 */
    ESP_LOGI(TAG, "cli ready (type help)");
    return ESP_OK;
}

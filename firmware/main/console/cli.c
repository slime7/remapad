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
#include "ble_controller.h"
#include "ble_session.h"
#include "buzzer.h"
#include "dp_source.h"
#include "ns2_identity.h"
#include "ns2_output.h"
#include "ota_session.h"
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
    cli_print("  pairing start|stop  sync key: drop link + discovery advertising");
    cli_print("  wake                force a reconnect of the paired console");
    cli_print("  adv wake|reconnect  steady form while paired (default wake)");
    cli_print("  report              dump the last input report actually sent");
    cli_print("  motion 0|1|2        0x09 motion block: zeros / stamp / none");
    cli_print("  ltk 0|1             LTK store form (0 reversed, 1 as-is)");
    cli_print("  drop                disconnect the current host");
    cli_print("  version             running image version, partition and ota state");
    cli_print("  rollback            roll back to the previous image (pending verify only)");
    cli_print("  poweroff            release power latch (battery only)");
    cli_print("  reboot              restart into COM mode");
}

static void cli_status(void)
{
    char line[224];
    const app_config_t *cfg = app_config_get();
    snprintf(line, sizeof(line),
             "state pairing=%s role=%s backlight=%u screen=%u uptime=%llds heap=%u "
             "batt=%umV/%u%% chg=%u fw=%s part=%s ota=%s",
             js_bridge_pairing_state(),
             cfg->usb_role == APP_CONFIG_USB_HOST ? "host" : "device",
             (unsigned)backlight_get(), (unsigned)cfg->screen_on,
             (long long)(esp_timer_get_time() / 1000000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)battery_get_voltage_mv(), (unsigned)battery_get_percentage(),
             battery_is_charging() ? 1u : 0u,
             ota_session_running_version(), ota_session_running_partition(),
             ota_session_state_name());
    cli_print(line);
}

/** 运行镜像信息：版本与分区来自 OTA 会话（与 UI 系统页同一来源）。 */
static void cli_version(void)
{
    char line[128];
    snprintf(line, sizeof(line), "fw=%s part=%s image=%s ota=%s",
             ota_session_running_version(), ota_session_running_partition(),
             ota_session_pending_verify() ? "pending-verify" : "confirmed",
             ota_session_state_name());
    cli_print(line);
}

/** 回滚演练：只有待验证镜像（OTA 后首次启动、尚未过健康门槛）能回滚。 */
static void cli_rollback(void)
{
    const esp_err_t err = ota_session_rollback_and_reboot();
    if (err == ESP_ERR_INVALID_STATE) {
        cli_print("err running image is not pending verification");
        return;
    }
    if (err != ESP_OK) {
        char line[64];
        snprintf(line, sizeof(line), "err rollback failed: %s", esp_err_to_name(err));
        cli_print(line);
        return;
    }
    /* 成功路径会直接重启，这行只在中断前送得出去的情况下可见。 */
    cli_print("ok rolling back to the previous image");
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

/** 广播形态短名：未在广播（已连接或未配对静默）时报 off。 */
static const char *link_adv_name(const ns2_session_status_t *status)
{
    if (!status->advertising) {
        return "off";
    }
    switch ((ns2_adv_mode_t)status->adv_mode) {
    case NS2_ADV_WAKE:
        return "wake";
    case NS2_ADV_RECONNECT:
        return "reconnect";
    default:
        return "discovery";
    }
}

/** BLE 链路观测：当前形态每个身份一行，含对外地址与上报计数。 */
static void cli_link(void)
{
    uint8_t ids[2] = {0};
    const size_t count = ns2_session_mode_identities(ids);
    char line[176];
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
            uint16_t mtu = 0;
            uint32_t tx_fail = 0;
            int tx_rc = 0;
            bool enc = false;
            ble_controller_conn_stats(status.conn_handle, NULL, &mtu, &tx_fail, &tx_rc, &enc);
            snprintf(line, sizeof(line),
                     "  %-4s %s conn=%u itvl=%u mtu=%u enc=%u fmt=0x%02x notify=%s%s "
                     "feat=%u reports=%lu txf=%lu/rc%d creds=%u adv=%s addr=%s motion=%u ltk=%u",
                     ns2_identity_name(status.identity), link_state_name(status.state),
                     status.conn_handle, status.conn_itvl, (unsigned)mtu, enc ? 1u : 0u,
                     status.report_format,
                     status.notify_05 ? "05" : "-", status.notify_09 ? "09" : "-",
                     status.features_enabled ? 1u : 0u,
                     (unsigned long)status.reports, (unsigned long)tx_fail, tx_rc,
                     (unsigned)status.creds, link_adv_name(&status), addr,
                     ns2_output_motion_mode(),
                     ns2_session_ltk_form());
        } else {
            snprintf(line, sizeof(line), "  %-4s %s creds=%u adv=%s addr=%s",
                     ns2_identity_name(status.identity), link_state_name(status.state),
                     (unsigned)status.creds, link_adv_name(&status), addr);
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

/** 唤醒请求：已配对设备常态就发唤醒形态（0x81），这里的动作是把链路重新
 *  走一遍——已连接就断开让主机按唤醒广播重连，未连接就重发一次广播。 */
static void cli_wake(void)
{
    ns2_session_wake_request();
    cli_print("ok reconnect requested");
}

/** 常态广播形态 A/B：实机对比唤醒（0x81，默认）与回连（0x00）两种形态。 */
static void cli_adv(const char *arg)
{
    if (strcmp(arg, "wake") == 0 || strcmp(arg, "reconnect") == 0) {
        ns2_session_set_steady_adv(arg[0] == 'w' ? NS2_ADV_WAKE : NS2_ADV_RECONNECT);
        cli_print("ok steady advertising updated");
    } else if (arg[0] == '\0') {
        cli_print(ns2_session_steady_adv() == NS2_ADV_WAKE
                      ? "steady advertising: wake (0x81)"
                      : "steady advertising: reconnect (0x00)");
    } else {
        cli_print("err usage: adv wake|reconnect");
    }
}

/** 抓线上输入报文：主机「已连接、已订阅但没有输入」时，用它确认设备真正
 *  发出去的字节（计数器是否递增、0x0E 运动长度、按键位、状态字节）。 */
static void cli_report(void)
{
    uint8_t ids[2] = {0};
    const size_t count = ns2_session_mode_identities(ids);
    for (size_t i = 0; i < count; i++) {
        ns2_session_status_t status;
        if (!ns2_session_status(ids[i], &status) || !status.connected) {
            continue;
        }
        uint8_t body[63];
        const uint8_t fmt = status.report_format == 5 ? 5 : 9;
        if (!ble_controller_last_input(status.conn_handle, fmt, body)) {
            cli_print("err no report sent yet");
            continue;
        }
        char line[176];
        snprintf(line, sizeof(line),
                 "  %-4s fmt=0x%02x cnt=%u pow=0x%02x btn=%02x%02x%02x L=%02x%02x%02x "
                 "R=%02x%02x%02x st=0x%02x nfc=0x%02x motion=0x%02x",
                 ns2_identity_name(status.identity), fmt, body[0], body[1], body[2], body[3],
                 body[4], body[5], body[6], body[7], body[8], body[9], body[10], body[0x0B],
                 body[0x0C], body[0x0E]);
        cli_print(line);
    }
}

/** 0x09 运动块占位切换：实机确认主机是否校验运动数据，无需重新烧录。 */
static void cli_motion(const char *arg)
{
    const int mode = atoi(arg);
    if (mode < NS2_MOTION_ZERO || mode > NS2_MOTION_NONE) {
        cli_print("err motion 0|1|2");
        return;
    }
    ns2_output_set_motion_mode((uint8_t)mode);
    char line[48];
    snprintf(line, sizeof(line), "ok motion=%d", mode);
    cli_print(line);
}

/** LTK 注入形态切换：主机连上但 link 显示 enc=0（未加密）时现场对比两种
 *  形态，判断是不是密钥字节序导致主机不认这台手柄。 */
static void cli_ltk(const char *arg)
{
    const int form = atoi(arg);
    if (form != 0 && form != 1) {
        cli_print("err ltk 0|1");
        return;
    }
    ns2_session_set_ltk_form((uint8_t)form);
    char line[48];
    snprintf(line, sizeof(line), "ok ltk_form=%d (next connect)", form);
    cli_print(line);
}

/** 断开当前主机：改完开关后用它让主机重新连接（重新走一遍注入）。 */
static void cli_drop(void)
{
    ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
    cli_print("ok disconnect requested");
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
    } else if (strcmp(line, "wake") == 0) {
        cli_wake();
    } else if (strcmp(line, "adv") == 0) {
        cli_adv(arg);
    } else if (strcmp(line, "report") == 0) {
        cli_report();
    } else if (strcmp(line, "motion") == 0) {
        cli_motion(arg);
    } else if (strcmp(line, "ltk") == 0) {
        cli_ltk(arg);
    } else if (strcmp(line, "drop") == 0) {
        cli_drop();
    } else if (strcmp(line, "version") == 0) {
        cli_version();
    } else if (strcmp(line, "rollback") == 0) {
        cli_rollback();
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

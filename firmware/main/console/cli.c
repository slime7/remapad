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
#include "console_out.h"
#include "dp_source.h"
#include "dp_ui.h"
#include "dp_plane.h"
#include "feedback.h"
#include "input_source.h"
#include "layout.h"
#include "ns2_identity.h"
#include "ns2_output.h"
#include "ota_session.h"
#include "pad_device.h"
#include "pad_state.h"
#include "pocketjs_host.h"
#include "target.h"
#include "usb_transport.h"
#include "usb_input.h"

static const char *TAG = "remapad_cli";

#define CLI_LINE_MAX 96

static void cli_print(const char *text)
{
    /* 输出走当前控制台通道：设备模式是 USJ 的非阻塞 vfs，host 模式是 UART0。 */
    console_out_write(text, strlen(text));
    console_out_write("\r\n", 2);
}

static void cli_help(void)
{
    cli_print("remapad cli commands:");
    cli_print("  status              system status one-liner");
    cli_print("  mem                 live memory snapshot (psram/internal/js, printed next frame)");
    cli_print("  key <name> [ms]     inject debug key (key release clears)");
    cli_print("                      a b x y plus minus home capture c l r zl zr");
    cli_print("                      ls rs up down left right gl gr ui");
    cli_print("  stick l|r <x> <y>   set stick level (0-4095, center)");
    cli_print("  stick reset         center both sticks");
    cli_print("  ui [on|off]         pad-captured screen control (no arg = state)");
    cli_print("  link                per-identity BLE link status");
    cli_print("  shot                capture the real screen to the PC (PNG on the PC side)");
    cli_print("  backlight [0-100]   set + persist backlight (no arg = current)");
    cli_print("  screen [on|off]     screen power (no arg = current)");
    cli_print("  beep [ms]           buzzer hint tone (default 120)");
    cli_print("  ctrl [pro|joycon [body button grip]]");
    cli_print("                      controller type + 0xRRGGBB colors, persisted (no arg = current)");
    cli_print("  mode device|host    usb connection mode");
    cli_print("  connect             connection key: advertising window (PWR long press)");
    cli_print("  pairing start|stop  pair a new host: drop link + discovery advertising");
    cli_print("  wake                open the wake window (drop link if connected)");
    cli_print("  adv auto|wake|reconnect");
    cli_print("                      form inside the advertising window (default auto)");
    cli_print("  report              dump the last input report actually sent");
    cli_print("  motion [0|1|2|3]    0x09 motion block (no arg = current)");
    cli_print("                      0 zeros / 1 stamp / 2 none / 3 sensor");
    cli_print("  headset [auto|0xNN] 0x09 headset byte sent to the host (auto = input device)");
    cli_print("  ltk [0|1]           LTK store form (no arg = current)");
    cli_print("  drop                disconnect the current host");
    cli_print("  pad                 recognized pad, layout row and relay state");
    cli_print("  usb                 usb host state (role, device, counters)");
    cli_print("  relay [0|1]         same-generation passthrough (default on, no arg = current)");
    cli_print("  rumble [off|l r]    manual rumble 0-255 per side (no arg = held state)");
    cli_print("  lamp [0x0-0xF]      manual player lamp mask (no arg = held state)");
    cli_print("  haptic [0xNN]       manual haptic sample byte (no arg = held state)");
    cli_print("  fwver [a.b.c]       handset fw version reported to the host");
    cli_print("  fwack [hex bytes]   ack body for the host update frame (default empty)");
    cli_print("  fwpost [a.b.c]      version reported after a host update (default 9.9.9)");
    cli_print("  fwapply [on|off]    arm the host-update reboot (one shot, default off)");
    cli_print("  version             running image version, partition and ota state");
    cli_print("  rollback            roll back to the previous image (pending verify only)");
    cli_print("  poweroff            release power latch (battery only)");
    cli_print("  reboot              restart into COM mode");
}

static void cli_status(void)
{
    char line[288];
    const app_config_t *cfg = app_config_get();
    snprintf(line, sizeof(line),
             "state pairing=%s role=%s backlight=%u screen=%u uptime=%llds heap=%u "
             "batt=%umV/%u%% chg=%u fw=%s part=%s ota=%s ui=%s pad=%s",
             js_bridge_pairing_state(),
             cfg->usb_role == APP_CONFIG_USB_HOST ? "host" : "device",
             (unsigned)backlight_get(), (unsigned)cfg->screen_on,
             (long long)(esp_timer_get_time() / 1000000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)battery_get_voltage_mv(), (unsigned)battery_get_percentage(),
             battery_is_charging() ? 1u : 0u,
             ota_session_running_version(), ota_session_running_partition(),
             ota_session_state_name(),
             dp_ui_active() ? "on" : "off",
             input_source_attached() ? input_source_device_desc()
                                     : (usb_input_attached() ? usb_input_device_desc() : "none"));
    cli_print(line);
}

/**
 * 实时内存全景：owner task 下一帧直接从引擎与堆账读数后经控制台出口回打。
 * 不经过 UI 层——截图冻结或系统页门控（隐藏时停止取数）都不影响这里的
 * 实时性，串口侧拿到的永远是发起那一刻的现场值。
 */
static void cli_mem(void)
{
    remapad_ui_request_mem();
    cli_print("ok mem report queued (printed next frame)");
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

/** 手柄操控 UI 模式：面板组合键之外的直接开关，实机上用它验证捕获与恢复。 */
static void cli_ui(const char *arg)
{
    if (arg[0] == '\0') {
        cli_print(dp_ui_active() ? "pad ui mode on (dpad moves, circle confirms)"
                                 : "pad ui mode off");
        return;
    }
    if (strcmp(arg, "on") == 0) {
        dp_ui_set_active(true);
        cli_print("ok pad ui mode on");
    } else if (strcmp(arg, "off") == 0) {
        dp_ui_set_active(false);
        cli_print("ok pad ui mode off");
    } else {
        cli_print("err usage: ui [on|off]");
    }
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

/** 广播形态短名：没在广播（设备静默、已连接或配对流程之外）时报 off。 */
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
    case NS2_ADV_OFF:
        return "off";
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
    if (arg[0] == '\0') {
        char line[48];
        snprintf(line, sizeof(line), "backlight %u", (unsigned)backlight_get());
        cli_print(line);
        return;
    }
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
    if (arg[0] == '\0') {
        cli_print(app_config_get()->screen_on ? "screen on" : "screen off");
        return;
    }
    if (strcmp(arg, "on") == 0) {
        js_bridge_screen_power(true);
        cli_print("ok screen on");
    } else if (strcmp(arg, "off") == 0) {
        js_bridge_screen_power(false);
        cli_print("ok screen off");
    } else {
        cli_print("err usage: screen [on|off]");
    }
}

/**
 * 手柄身份配置（与 UI 手柄设置页同一条持久化路径）：无参回读当前形态与
 * 三处配色；带形态名则改形态（配色沿用现值），再带 0xRRGGBB 三元组则一并
 * 改配色。写 NVS 由提交任务按周期落盘，BLE 侧经 ns2_session_set_identity
 * 即时生效（下次广播/握手带新出厂块）。
 */
static void cli_ctrl(const char *arg)
{
    const app_config_t *cfg = app_config_get();
    if (arg[0] == '\0') {
        char line[128];
        snprintf(line, sizeof(line), "ctrl type=%s body=0x%06x button=0x%06x grip=0x%06x",
                 cfg->ctrl_type == APP_CONFIG_CTRL_JOYCON ? "joycon" : "pro",
                 (unsigned)cfg->body_color, (unsigned)cfg->button_color,
                 (unsigned)cfg->grip_color);
        cli_print(line);
        return;
    }
    char type[16] = {0};
    char body_text[16] = {0};
    char button_text[16] = {0};
    char grip_text[16] = {0};
    const int fields = sscanf(arg, "%15s %15s %15s %15s", type, body_text, button_text,
                              grip_text);
    bool joycon;
    if (strcmp(type, "joycon") == 0) {
        joycon = true;
    } else if (strcmp(type, "pro") == 0) {
        joycon = false;
    } else {
        cli_print("err usage: ctrl [pro|joycon [body button grip 0xRRGGBB]]");
        return;
    }
    uint32_t colors[3] = {cfg->body_color, cfg->button_color, cfg->grip_color};
    const char *color_text[3] = {body_text, button_text, grip_text};
    for (int i = 0; i < fields - 1; i++) {
        char *end = NULL;
        const unsigned long value = strtoul(color_text[i], &end, 0);
        if (end == color_text[i] || *end != '\0' || value > 0xFFFFFFu) {
            cli_print("err colors are 0xRRGGBB");
            return;
        }
        colors[i] = (uint32_t)value;
    }
    app_config_set_controller(joycon ? APP_CONFIG_CTRL_JOYCON : APP_CONFIG_CTRL_PRO,
                              colors[0], colors[1], colors[2]);
    ns2_session_set_identity(joycon, colors[0], colors[1], colors[2]);
    char line[96];
    snprintf(line, sizeof(line), "ok ctrl type=%s body=0x%06x button=0x%06x grip=0x%06x",
             joycon ? "joycon" : "pro", (unsigned)colors[0], (unsigned)colors[1],
             (unsigned)colors[2]);
    cli_print(line);
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
        cli_print("ok pair-new-host queued (drop link + discovery advertising)");
    } else if (strcmp(arg, "stop") == 0) {
        js_bridge_submit_command("{\"t\":\"disconnect\",\"id\":0}");
        cli_print("ok advertising stop queued (drop link, silent)");
    } else {
        cli_print("err usage: pairing start|stop");
    }
}

/** 连接键：打开连接窗口（已配对发回连形态等主机连回来，未配对进配对流程）。
 *  与 PWR 长按、屏幕「连接」按钮同一条路径。 */
static void cli_connect(void)
{
    js_bridge_submit_command("{\"t\":\"connect\",\"id\":0}");
    cli_print("ok connect queued (connection window)");
}

/** 唤醒请求：打开唤醒窗口（未连接时发 0x81 把休眠主机叫起来），已连接就
 *  断开让主机按唤醒广播重连一次。 */
static void cli_wake(void)
{
    ns2_session_wake_request();
    cli_print("ok wake window opened");
}

/** 广播窗口内形态的 A/B：auto 按窗口来源决策（默认，连接键回连、HOME 唤醒），
 *  wake/reconnect 钉住窗口内的已配对形态做实机对账。 */
static void cli_adv(const char *arg)
{
    if (strcmp(arg, "auto") == 0) {
        ns2_session_set_window_form(NS2_WINDOW_FORM_AUTO);
        cli_print("ok window form: auto");
    } else if (strcmp(arg, "wake") == 0) {
        ns2_session_set_window_form(NS2_WINDOW_FORM_WAKE);
        cli_print("ok window form: wake (0x81)");
    } else if (strcmp(arg, "reconnect") == 0) {
        ns2_session_set_window_form(NS2_WINDOW_FORM_RECONNECT);
        cli_print("ok window form: reconnect (0x00)");
    } else if (arg[0] == '\0') {
        switch (ns2_session_window_form()) {
        case NS2_WINDOW_FORM_WAKE:
            cli_print("window form: wake (0x81)");
            break;
        case NS2_WINDOW_FORM_RECONNECT:
            cli_print("window form: reconnect (0x00)");
            break;
        default:
            cli_print("window form: auto (window request)");
            break;
        }
    } else {
        cli_print("err usage: adv auto|wake|reconnect");
    }
}

/** 抓线上输入报文：主机「已连接、已订阅但没有输入」时，用它确认设备真正
 *  发出去的字节（计数器是否递增、0x0E 运动长度、按键位、状态字节）。 */
static void cli_report(void)
{
    uint8_t ids[2] = {0};
    const size_t count = ns2_session_mode_identities(ids);
    bool printed = false;
    for (size_t i = 0; i < count; i++) {
        ns2_session_status_t status;
        if (!ns2_session_status(ids[i], &status) || !status.connected) {
            continue;
        }
        uint8_t body[63];
        const uint8_t fmt = status.report_format == 5 ? 5 : 9;
        if (!ble_controller_last_input(status.conn_handle, fmt, body)) {
            cli_print("err no report sent yet");
            printed = true;
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
        printed = true;
    }
    if (!printed) {
        cli_print("report none (no connected host)");
    }
}

/** 0x09 运动块内容切换：0 全零 / 1 抓包占位 / 2 不带 / 3 输入设备的真实样本。 */
static void cli_motion(const char *arg)
{
    if (arg[0] == '\0') {
        char line[32];
        snprintf(line, sizeof(line), "motion %u", (unsigned)ns2_output_motion_mode());
        cli_print(line);
        return;
    }
    const int mode = atoi(arg);
    if (mode < NS2_MOTION_ZERO || mode > NS2_MOTION_SENSOR) {
        cli_print("err motion 0|1|2|3");
        return;
    }
    ns2_output_set_motion_mode((uint8_t)mode);
    char line[48];
    snprintf(line, sizeof(line), "ok motion=%d", mode);
    cli_print(line);
}

/**
 * 实机截图：请求交给 PocketJS owner task 在下一帧把整幅画面重渲染一遍，
 * 像素经桥接图像帧回传；PC 侧（pc/remapadctl.py 的 shot）落地成 PNG。
 * 这里只置标志，不等回传，回复 ok 表示请求已入队。
 */
static void cli_shot(void)
{
    remapad_ui_request_shot();
    cli_print("ok shot queued (PC side saves the PNG)");
}

/**
 * 3.5mm 耳机状态字节（0x09 的 0x0D 与 0x05 的插入位）：auto 用输入设备
 * 派生的值，0xNN 钉住一个取值做主机侧 A/B（例如 controller.md 里的 0x0D）。
 * 不带参数回显覆盖开关、覆盖值与 auto 派生值。
 */
static void cli_headset(const char *arg)
{
    char line[96];
    if (arg[0] == '\0') {
        uint8_t override_value = 0;
        if (ns2_output_headset_override(&override_value)) {
            snprintf(line, sizeof(line), "headset override 0x%02x (auto value 0x%02x)",
                     (unsigned)override_value, (unsigned)ns2_output_headset_derived());
        } else {
            snprintf(line, sizeof(line), "headset auto (value 0x%02x)",
                     (unsigned)ns2_output_headset_derived());
        }
        cli_print(line);
        return;
    }
    if (strcmp(arg, "auto") == 0) {
        ns2_output_set_headset_override(false, 0);
        cli_print("ok headset auto (derived from the input device)");
        return;
    }
    char *end = NULL;
    const unsigned long value = strtoul(arg, &end, 16);
    if (end == arg || *end != '\0' || value > 0xFF) {
        cli_print("err usage: headset auto|0xNN");
        return;
    }
    ns2_output_set_headset_override(true, (uint8_t)value);
    snprintf(line, sizeof(line), "ok headset override 0x%02lx", value);
    cli_print(line);
}

/** LTK 注入形态切换：主机连上但 link 显示 enc=0（未加密）时现场对比两种
 *  形态，判断是不是密钥字节序导致主机不认这台手柄。 */
static void cli_ltk(const char *arg)
{
    if (arg[0] == '\0') {
        char line[32];
        snprintf(line, sizeof(line), "ltk_form %u", (unsigned)ns2_session_ltk_form());
        cli_print(line);
        return;
    }
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

/** 当前持续反馈帧一行：叠加后的震动、玩家灯与触觉采样（无参回读共用）。 */
static void cli_feedback_state(void)
{
    pad_feedback_t held;
    dp_plane_feedback_held(&held);
    char haptic[12];
    if (held.haptic_sample_valid) {
        snprintf(haptic, sizeof(haptic), "0x%02x", (unsigned)held.haptic_sample);
    } else {
        snprintf(haptic, sizeof(haptic), "none");
    }
    char line[128];
    snprintf(line, sizeof(line),
             "feedback rumble l=%u r=%u lamp=0x%x haptic=%s",
             (unsigned)held.rumble_strength[PAD_TRIGGER_L2],
             (unsigned)held.rumble_strength[PAD_TRIGGER_R2],
             (unsigned)held.player_led, haptic);
    cli_print(line);
}

/**
 * 手动反馈注入：把事件叠加进持续帧后由数据面按接入设备的布局编码投递，
 * 与主机下发的反馈走完全同一条路径（USB 直插走 OUT 端点，桥接路径回传 PC
 * 写手柄）。没有主机在场时，串口脚本用它验证震动 / 玩家灯 / 触觉采样整条
 * 反馈链路。震动手动注入按归一强度（0-255）走；同代 NS2 手柄吃主机的原始
 * LRA 参数包，这条命令对它只能写停（全零包）。
 */
static void cli_rumble(const char *arg)
{
    if (arg[0] == '\0') {
        cli_feedback_state();
        return;
    }
    if (strcmp(arg, "off") == 0) {
        pad_feedback_t off;
        pad_feedback_defaults(&off);
        dp_plane_inject_feedback(PAD_FEEDBACK_FIELD_RUMBLE, &off);
        cli_print("ok rumble off");
        return;
    }
    char left[8] = {0};
    char right[8] = {0};
    if (sscanf(arg, "%7s %7s", left, right) != 2) {
        cli_print("err usage: rumble [off|<l 0-255> <r 0-255>]");
        return;
    }
    char *left_end = NULL;
    char *right_end = NULL;
    const unsigned long l = strtoul(left, &left_end, 0);
    const unsigned long r = strtoul(right, &right_end, 0);
    if (left_end == left || *left_end != '\0' || l > 255 || right_end == right ||
        *right_end != '\0' || r > 255) {
        cli_print("err usage: rumble [off|<l 0-255> <r 0-255>]");
        return;
    }
    pad_feedback_t event;
    pad_feedback_defaults(&event);
    event.rumble_on[PAD_TRIGGER_L2] = l > 0;
    event.rumble_on[PAD_TRIGGER_R2] = r > 0;
    event.rumble_strength[PAD_TRIGGER_L2] = (uint8_t)l;
    event.rumble_strength[PAD_TRIGGER_R2] = (uint8_t)r;
    dp_plane_inject_feedback(PAD_FEEDBACK_FIELD_RUMBLE, &event);
    char line[48];
    snprintf(line, sizeof(line), "ok rumble l=%lu r=%lu", l, r);
    cli_print(line);
}

/** 玩家灯手动注入：bit0-3 掩码，0 全灭。 */
static void cli_lamp(const char *arg)
{
    if (arg[0] == '\0') {
        cli_feedback_state();
        return;
    }
    char *end = NULL;
    const unsigned long mask = strtoul(arg, &end, 0);
    if (end == arg || *end != '\0' || mask > 0x0Fu) {
        cli_print("err usage: lamp [0x0-0xF]");
        return;
    }
    pad_feedback_t event;
    pad_feedback_defaults(&event);
    event.player_led = (uint8_t)mask;
    dp_plane_inject_feedback(PAD_FEEDBACK_FIELD_PLAYER_LED, &event);
    char line[32];
    snprintf(line, sizeof(line), "ok lamp 0x%lx", mask);
    cli_print(line);
}

/** 触觉采样手动注入：0x00 合法（主机用它收掉提示音）。 */
static void cli_haptic(const char *arg)
{
    if (arg[0] == '\0') {
        cli_feedback_state();
        return;
    }
    char *end = NULL;
    const unsigned long sample = strtoul(arg, &end, 0);
    if (end == arg || *end != '\0' || sample > 0xFF) {
        cli_print("err usage: haptic [0xNN]");
        return;
    }
    pad_feedback_t event;
    pad_feedback_defaults(&event);
    event.haptic_sample_valid = true;
    event.haptic_sample = (uint8_t)sample;
    dp_plane_inject_feedback(PAD_FEEDBACK_FIELD_HAPTIC, &event);
    char line[40];
    snprintf(line, sizeof(line), "ok haptic 0x%02lx", sample);
    cli_print(line);
}

static const char *cli_conn_name(pad_conn_t conn)
{
    switch (conn) {
    case PAD_CONN_USB:
        return "usb";
    case PAD_CONN_BT:
        return "bt";
    default:
        return "-";
    }
}

/** 识别结果一行：来源、家族、型号、命中的布局行、兜底与透传状态。 */
static void cli_pad(void)
{
    char line[224];
    uint16_t vid = 0;
    uint16_t pid = 0;
    pad_conn_t conn = PAD_CONN_UNKNOWN;
    const char *source = "none";
    if (input_source_device_ids(&vid, &pid, &conn)) {
        source = "bridge";
    } else if (usb_input_device_ids(&vid, &pid, &conn)) {
        source = "usb";
    }
    if (strcmp(source, "none") == 0) {
        cli_print("pad none");
        return;
    }
    pad_family_t family = PAD_FAMILY_UNKNOWN;
    const pad_layout_t *layout = pad_layout_find_by_ids(vid, pid, conn, &family);
    snprintf(line, sizeof(line),
             "pad %s %s %s %04x:%04x row=0x%02x native=%u fallback=%u relay=%u", source,
             pad_family_name(family), cli_conn_name(conn), (unsigned)vid, (unsigned)pid,
             layout != NULL ? (unsigned)layout->report_id : 0u,
             layout != NULL ? (unsigned)layout->native_lang : 0u, layout == NULL ? 1u : 0u,
             target_relay_enabled() ? 1u : 0u);
    cli_print(line);
}

/** USB host 状态：角色、栈状态、设备与收发计数、日志出口。 */
static void cli_usb(void)
{
    char line[192];
    snprintf(line, sizeof(line), "usb role=%s stack=%u device=%s reports=%lu outputs=%lu console=%s",
             app_config_get()->usb_role == APP_CONFIG_USB_HOST ? "host" : "device",
             usb_host_running() ? 1u : 0u, usb_input_device_desc(),
             (unsigned long)usb_input_report_count(), (unsigned long)usb_input_output_count(),
             console_out_uart_active() ? "uart0" : "usj");
    cli_print(line);
}

/** 同代透传开关：0 关、1 开（默认开）；无参回读当前值。 */
static void cli_relay(const char *arg)
{
    if (arg[0] == '\0') {
        char line[32];
        snprintf(line, sizeof(line), "relay %u", target_relay_enabled() ? 1u : 0u);
        cli_print(line);
        return;
    }
    if (arg[0] != '0' && arg[0] != '1') {
        cli_print("err relay [0|1]");
        return;
    }
    target_set_relay(arg[0] == '1');
    char line[32];
    snprintf(line, sizeof(line), "ok relay=%c", arg[0]);
    cli_print(line);
}

/** 解析 a.b.c 版本串（每段 0-255）。 */
static bool parse_version(const char *text, uint8_t out[3])
{
    unsigned major = 0;
    unsigned minor = 0;
    unsigned revision = 0;
    if (sscanf(text, "%u.%u.%u", &major, &minor, &revision) != 3) {
        return false;
    }
    if (major > 255 || minor > 255 || revision > 255) {
        return false;
    }
    out[0] = (uint8_t)major;
    out[1] = (uint8_t)minor;
    out[2] = (uint8_t)revision;
    return true;
}

/** 上报给主机的手柄固件版本（0x10 查询与两个出厂块共用；假升级会话完成时
 *  自行递增）。实机对账用：把版本抬到主机认为无需更新的值，或复位到出厂
 *  版本再走一次主机的更新流程。 */
static void cli_fwver(const char *arg)
{
    char line[64];
    const app_config_t *cfg = app_config_get();
    if (arg[0] == '\0') {
        snprintf(line, sizeof(line), "handset fw %u.%u.%u (reported to host)",
                 cfg->fw_version[0], cfg->fw_version[1], cfg->fw_version[2]);
        cli_print(line);
        return;
    }
    uint8_t ver[3];
    if (!parse_version(arg, ver)) {
        cli_print("err usage: fwver <major>.<minor>.<revision>");
        return;
    }
    app_config_set_fw_version(ver);
    ns2_session_refresh_fw_version();
    snprintf(line, sizeof(line), "ok handset fw -> %u.%u.%u", ver[0], ver[1], ver[2]);
    cli_print(line);
}

/** 假升级收尾后上报的版本：主机拿它判断还要不要再推一次更新。 */
static void cli_fwpost(const char *arg)
{
    char line[64];
    uint8_t ver[3];
    if (arg[0] == '\0') {
        ns2_session_fw_post_version(ver);
        snprintf(line, sizeof(line), "fw upgrade post version %u.%u.%u", ver[0], ver[1], ver[2]);
        cli_print(line);
        return;
    }
    if (!parse_version(arg, ver)) {
        cli_print("err usage: fwpost <major>.<minor>.<revision>");
        return;
    }
    ns2_session_set_fw_post_version(ver);
    snprintf(line, sizeof(line), "ok fw upgrade post version -> %u.%u.%u", ver[0], ver[1], ver[2]);
    cli_print(line);
}

/** 升级帧应答体：主机更新流程无公开文档，现场替换做 A/B（空体 = 只回帧头）。 */
static void cli_fwack(const char *arg)
{
    char line[96];
    uint8_t body[16];
    if (arg[0] == '\0') {
        const size_t len = ns2_session_fw_ack_body(body, sizeof(body));
        int used = snprintf(line, sizeof(line), "fw upgrade ack body (%uB):", (unsigned)len);
        for (size_t i = 0; i < len && used > 0 && (size_t)used + 4 < sizeof(line); i++) {
            used += snprintf(&line[used], sizeof(line) - (size_t)used, " %02x", body[i]);
        }
        cli_print(line);
        return;
    }
    size_t len = 0;
    const char *p = arg;
    while (*p != '\0' && len < sizeof(body)) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *end = NULL;
        const unsigned long byte = strtoul(p, &end, 16);
        if (end == p || byte > 0xFF) {
            cli_print("err usage: fwack [hex bytes]  e.g. fwack 01 00");
            return;
        }
        body[len++] = (uint8_t)byte;
        p = end;
    }
    ns2_session_set_fw_ack_body(body, len);
    snprintf(line, sizeof(line), "ok fw upgrade ack body -> %uB", (unsigned)len);
    cli_print(line);
}

/** 假升级收尾动作：主机推完更新后会等控制器重启回来。实测重启会被主机当成
 *  更新没生效而重推整包（推包→重启→再推包），所以默认不重启；这里只做
 *  一次性武装，留给实机对账那一次。 */
static void cli_fwapply(const char *arg)
{
    char line[64];
    if (arg[0] == '\0') {
        snprintf(line, sizeof(line), "fw upgrade reboot armed=%u",
                 ns2_session_fw_restart_armed() ? 1u : 0u);
        cli_print(line);
        return;
    }
    if (strcmp(arg, "on") == 0) {
        ns2_session_set_fw_restart_armed(true);
        cli_print("ok next host update applies the post version and reboots");
        return;
    }
    if (strcmp(arg, "off") == 0) {
        ns2_session_set_fw_restart_armed(false);
        cli_print("ok host update no longer reboots");
        return;
    }
    cli_print("err usage: fwapply on|off");
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
    } else if (strcmp(line, "mem") == 0) {
        cli_mem();
    } else if (strcmp(line, "key") == 0) {
        cli_key(arg);
    } else if (strcmp(line, "stick") == 0) {
        cli_stick(arg);
    } else if (strcmp(line, "ui") == 0) {
        cli_ui(arg);
    } else if (strcmp(line, "link") == 0) {
        cli_link();
    } else if (strcmp(line, "backlight") == 0) {
        cli_backlight(arg);
    } else if (strcmp(line, "screen") == 0) {
        cli_screen(arg);
    } else if (strcmp(line, "ctrl") == 0) {
        cli_ctrl(arg);
    } else if (strcmp(line, "beep") == 0) {
        cli_beep(arg);
    } else if (strcmp(line, "mode") == 0) {
        cli_mode(arg);
    } else if (strcmp(line, "pairing") == 0) {
        cli_pairing(arg);
    } else if (strcmp(line, "connect") == 0) {
        cli_connect();
    } else if (strcmp(line, "wake") == 0) {
        cli_wake();
    } else if (strcmp(line, "adv") == 0) {
        cli_adv(arg);
    } else if (strcmp(line, "report") == 0) {
        cli_report();
    } else if (strcmp(line, "motion") == 0) {
        cli_motion(arg);
    } else if (strcmp(line, "shot") == 0) {
        cli_shot();
    } else if (strcmp(line, "headset") == 0) {
        cli_headset(arg);
    } else if (strcmp(line, "pad") == 0) {
        cli_pad();
    } else if (strcmp(line, "usb") == 0) {
        cli_usb();
    } else if (strcmp(line, "relay") == 0) {
        cli_relay(arg);
    } else if (strcmp(line, "rumble") == 0) {
        cli_rumble(arg);
    } else if (strcmp(line, "lamp") == 0) {
        cli_lamp(arg);
    } else if (strcmp(line, "haptic") == 0) {
        cli_haptic(arg);
    } else if (strcmp(line, "ltk") == 0) {
        cli_ltk(arg);
    } else if (strcmp(line, "drop") == 0) {
        cli_drop();
    } else if (strcmp(line, "fwver") == 0) {
        cli_fwver(arg);
    } else if (strcmp(line, "fwack") == 0) {
        cli_fwack(arg);
    } else if (strcmp(line, "fwpost") == 0) {
        cli_fwpost(arg);
    } else if (strcmp(line, "fwapply") == 0) {
        cli_fwapply(arg);
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

#include "cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "app_config.h"
#include "backlight.h"
#include "bridge/js_bridge.h"
#include "buzzer.h"
#include "dp_source.h"
#include "ns2_state.h"

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
    cli_print("  key a|home|lr       inject debug key");
    cli_print("  backlight 0-100     set + persist backlight");
    cli_print("  screen on|off       screen power");
    cli_print("  beep [ms]           buzzer hint tone (default 120)");
    cli_print("  mode device|host    usb connection mode");
    cli_print("  pairing start|stop  pairing advertising");
    cli_print("  reboot              restart into COM mode");
}

static void cli_status(void)
{
    char line[128];
    const app_config_t *cfg = app_config_get();
    snprintf(line, sizeof(line),
             "state pairing=%s role=%s backlight=%u screen=%u uptime=%llds heap=%u",
             js_bridge_pairing_state(),
             cfg->usb_role == APP_CONFIG_USB_HOST ? "host" : "device",
             (unsigned)backlight_get(), (unsigned)cfg->screen_on,
             (long long)(esp_timer_get_time() / 1000000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cli_print(line);
}

static void cli_key(const char *arg)
{
    uint32_t mask = 0;
    uint32_t hold_ms = 250;
    if (strcmp(arg, "a") == 0) {
        mask = NS2_BTN_A;
    } else if (strcmp(arg, "home") == 0) {
        mask = NS2_BTN_HOME;
    } else if (strcmp(arg, "lr") == 0) {
        mask = NS2_BTN_L | NS2_BTN_R;
        hold_ms = 1000;
    }
    if (mask == 0) {
        cli_print("err unknown key (a|home|lr)");
        return;
    }
    dp_source_inject(mask, hold_ms);
    cli_print("ok key injected");
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
    } else if (strcmp(line, "reboot") == 0) {
        js_bridge_submit_command("{\"t\":\"reboot\",\"id\":0}");
        cli_print("ok reboot queued");
    } else if (line[0] != '\0') {
        cli_print("err unknown command, try help");
    }
}

static void cli_task(void *param)
{
    (void)param;
    /* USJ 为初级控制台时启动代码已注册 vfs；显式切到非阻塞模式：日志在
     * 未连接时丢弃（不会卡住任务），输入由本任务轮询读取。 */
    usb_serial_jtag_vfs_use_nonblocking();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);

    char line[CLI_LINE_MAX];
    size_t len = 0;
    ESP_LOGI(TAG, "cli ready (type help)");
    for (;;) {
        char ch;
        const ssize_t n = read(0, &ch, 1);
        if (n == 1) {
            if (ch == '\r' || ch == '\n') {
                line[len] = '\0';
                cli_dispatch(line);
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = ch;
            } else {
                cli_print("err line too long");
                len = 0;
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t remapad_cli_start(void)
{
    if (xTaskCreate(cli_task, "remapad-cli", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

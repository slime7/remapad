/**
 * ui_service 的状态源与控制面替身：电池、背光、NS2 会话、USB 输入/角色、
 * PC 链路、OTA 进度、BLE 栈开关、js_bridge 与控制台出口。只参与编译接线，
 * host_test_set_* 控制读数，host_test_js_bridge_* 捕获提交的命令与亮度。
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "backlight.h"
#include "ble_session.h"
#include "console_out.h"
#include "esp_app_desc.h"
#include "input_link.h"
#include "bridge/js_bridge.h"
#include "ota_session.h"
#include "usb_input.h"
#include "usb_role.h"

static struct {
  uint32_t battery_mv;
  uint8_t battery_pct;
  uint8_t backlight;
  bool ns2_registered;
  bool ns2_waiting;
  bool ns2_pairing_mode;
  bool ns2_advertising;
  bool ns2_paired;
  uint8_t ns2_player_leds;
  bool ns2_identity_valid;
  bool usb_attached;
  uint16_t usb_vid;
  uint16_t usb_pid;
  bool usb_host;
  bool link_active;
  bool link_pc;
  bool ble_running;
  int ota_phase;
  int ota_percent;
  size_t ota_ui_ready;
} s_stub;

static struct {
  char commands[16][160];
  size_t command_count;
  int last_brightness;
} s_bridge;

void host_test_set_battery(uint32_t voltage_mv, uint8_t percentage)
{
  s_stub.battery_mv = voltage_mv;
  s_stub.battery_pct = percentage;
}

void host_test_set_backlight(uint8_t pct)
{
  s_stub.backlight = pct;
}

void host_test_set_ns2_pairing(bool registered, bool waiting, bool pairing_mode, bool advertising, bool paired)
{
  s_stub.ns2_registered = registered;
  s_stub.ns2_waiting = waiting;
  s_stub.ns2_pairing_mode = pairing_mode;
  s_stub.ns2_advertising = advertising;
  s_stub.ns2_paired = paired;
}

void host_test_set_ns2_player_leds(uint8_t leds)
{
  s_stub.ns2_player_leds = leds;
}

void host_test_set_ns2_identity(bool valid)
{
  s_stub.ns2_identity_valid = valid;
}

void host_test_set_usb_input(bool attached, uint16_t vid, uint16_t pid)
{
  s_stub.usb_attached = attached;
  s_stub.usb_vid = vid;
  s_stub.usb_pid = pid;
}

void host_test_set_usb_role_host(bool host)
{
  s_stub.usb_host = host;
}

void host_test_set_pc_link(bool active, bool pc_connected)
{
  s_stub.link_active = active;
  s_stub.link_pc = pc_connected;
}

void host_test_set_ble_stack_running(bool running)
{
  s_stub.ble_running = running;
}

void host_test_set_ota_progress(int phase, int percent)
{
  s_stub.ota_phase = phase;
  s_stub.ota_percent = percent;
}

size_t host_test_ota_ui_ready_count(void)
{
  return s_stub.ota_ui_ready;
}

void host_test_ota_ui_ready_reset(void)
{
  s_stub.ota_ui_ready = 0;
}

void host_test_js_bridge_reset(void)
{
  s_bridge.command_count = 0;
  s_bridge.last_brightness = -1;
}

size_t host_test_js_bridge_command_count(void)
{
  return s_bridge.command_count;
}

const char *host_test_js_bridge_command(size_t index)
{
  if (index >= s_bridge.command_count) {
    return NULL;
  }
  return s_bridge.commands[index];
}

int host_test_js_bridge_last_brightness(void)
{
  return s_bridge.last_brightness;
}

uint32_t battery_get_voltage_mv(void)
{
  return s_stub.battery_mv;
}

uint8_t battery_get_percentage(void)
{
  return s_stub.battery_pct;
}

bool battery_is_charging(void)
{
  return false;
}

uint8_t backlight_get(void)
{
  return s_stub.backlight;
}

bool ns2_session_host_registered(void)
{
  return s_stub.ns2_registered;
}

bool ns2_session_waiting_pair(void)
{
  return s_stub.ns2_waiting;
}

bool ns2_session_pairing_mode_active(void)
{
  return s_stub.ns2_pairing_mode;
}

bool ns2_session_advertising(void)
{
  return s_stub.ns2_advertising;
}

bool ns2_session_paired(void)
{
  return s_stub.ns2_paired;
}

uint8_t ns2_session_player_leds(void)
{
  return s_stub.ns2_player_leds;
}

bool ns2_session_identity_mac(uint8_t identity, uint8_t out[6])
{
  (void)identity;
  if (!s_stub.ns2_identity_valid) {
    return false;
  }
  const uint8_t mac[6] = { 0x9C, 0xE6, 0x35, 0x11, 0x22, 0x33 };
  memcpy(out, mac, sizeof(mac));
  return true;
}

bool usb_input_attached(void)
{
  return s_stub.usb_attached;
}

bool usb_input_device_ids(uint16_t *vid, uint16_t *pid, pad_conn_t *conn)
{
  if (!s_stub.usb_attached) {
    return false;
  }
  if (vid != NULL) {
    *vid = s_stub.usb_vid;
  }
  if (pid != NULL) {
    *pid = s_stub.usb_pid;
  }
  if (conn != NULL) {
    memset(conn, 0, sizeof(*conn));
  }
  return true;
}

bool usb_role_host_active(void)
{
  return s_stub.usb_host;
}

bool input_link_active(void)
{
  return s_stub.link_active;
}

bool input_link_pc_connected(void)
{
  return s_stub.link_pc;
}

bool ble_controller_running(void)
{
  return s_stub.ble_running;
}

void ota_session_progress(int *phase, int *percent)
{
  if (phase != NULL) {
    *phase = s_stub.ota_phase;
  }
  if (percent != NULL) {
    *percent = s_stub.ota_percent;
  }
}

void ota_session_notify_ui_ready(void)
{
  s_stub.ota_ui_ready++;
}

esp_err_t js_bridge_submit_command(const char *cmd_json)
{
  if (s_bridge.command_count < 16) {
    snprintf(s_bridge.commands[s_bridge.command_count], sizeof(s_bridge.commands[0]), "%s", cmd_json);
    s_bridge.command_count++;
  }
  return ESP_OK;
}

void js_bridge_set_brightness(int brightness)
{
  /* 真实 setter 会落到背光：替身同步读数，亮度加减的连续动作才能读到新值。 */
  s_stub.backlight = (uint8_t)brightness;
  s_bridge.last_brightness = brightness;
}

const esp_app_desc_t *esp_app_get_description(void)
{
  static const esp_app_desc_t desc = {
    .magic_word = 0xABCD5432,
    .version = "1.2.3-test",
    .project_name = "remapad",
  };
  return &desc;
}

void console_out_write(const char *data, size_t len)
{
  (void)data;
  (void)len;
}

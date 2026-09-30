#include "netlog.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_phy_init.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "amiibo_session.h"
#include "app_config.h"
#include "cli.h"
#include "console_out.h"
#include "input_frame.h"
#include "input_link.h"
#include "input_source.h"
#include "netlog_retry.h"
#include "ota_link.h"

static const char *TAG = "remapad_netlog";

#define NETLOG_RX_BUF 1400
#define NETLOG_RX_TASK_STACK 8192
#define NETLOG_RX_TASK_PRIO 4
#define NETLOG_RX_POLL_MS 200
/** 桥接在位的静默窗：最近这么久内有报文往来才算活跃（反馈帧跟着输入端走）。 */
#define NETLOG_BRIDGE_WINDOW_US (2LL * 1000 * 1000)
/** STA 发射功率上限（0.25dBm 单位）：本板裸片功放 20dBm 时发射波形失真，任何 AP
 *  都解不出认证帧；实测 17dBm 尚可、17.5dBm 起临界，默认 15dBm 留裕量。 */
#define NETLOG_TX_POWER_QUARTER_DBM 60

typedef enum {
  NETLOG_STATE_OFF = 0,
  NETLOG_STATE_CONNECTING,
  NETLOG_STATE_CONNECTED,
} netlog_state_t;

static volatile bool s_running;
static volatile netlog_state_t s_state;
static volatile bool s_in_sink;
static char s_ssid[NETLOG_SSID_MAX];
static char s_pass[NETLOG_PASS_MAX];
static char s_addr_text[NETLOG_HOST_MAX];
static struct sockaddr_in s_dest;
static volatile bool s_dest_set;
static uint16_t s_port;
static int s_sock = -1;
static TaskHandle_t s_rx_task;
static input_frame_rx_t s_frame_rx;
static volatile int64_t s_last_rx_us;
static volatile uint32_t s_sent;
static volatile uint32_t s_dropped;
static volatile uint32_t s_frames;
static esp_netif_t *s_sta_netif;
static bool s_netif_ready;
static bool s_wifi_ready;
static bool s_wifi_started;
static volatile bool s_scan_hold;
static volatile bool s_scan_done;

#define NETLOG_SCAN_MAX 12
static wifi_ap_record_t s_scan_records[NETLOG_SCAN_MAX];
static int s_scan_count;

static esp_timer_handle_t s_reconnect_timer;
static volatile int s_fail_count;

static void reconnect_timer_cb(void *arg)
{
  (void)arg;
  if (!s_running || s_scan_hold) {
    return;
  }
  /* 连不上就不再扫射：整段关闭（射频断电），要再连只能手动开。 */
  if (netlog_retry_give_up(s_fail_count)) {
    ESP_LOGW(TAG, "giving up after %d failed connects, session closed", (int)s_fail_count);
    netlog_stop();
    return;
  }
  esp_wifi_connect();
}

static const char *state_name(void)
{
  switch (s_state) {
  case NETLOG_STATE_CONNECTING:
    return "connecting";
  case NETLOG_STATE_CONNECTED:
    return "connected";
  default:
    return "off";
  }
}

/** WiFi 事件：断线自动重连（会话开着才算，扫描期间压住）——按失败次数退避，
 *  累计 10 次失败转关闭；拿到 IP 才置连通并把失败计数清零。 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (!s_running || s_scan_hold) {
      return;
    }
    s_state = NETLOG_STATE_CONNECTING;
    s_fail_count++;
    const wifi_event_sta_disconnected_t *disconnected = data;
    const int64_t delay_us = netlog_retry_delay_us(s_fail_count);
    ESP_LOGW(TAG, "wifi disconnected reason=%u rssi=%d, retry %d/%d in %ds", (unsigned)disconnected->reason,
             (int)disconnected->rssi, (int)s_fail_count, NETLOG_FAIL_GIVE_UP, (int)(delay_us / 1000000LL));
    if (s_reconnect_timer != NULL) {
      esp_timer_stop(s_reconnect_timer);
      esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_us);
    }
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    s_scan_done = true;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = data;
    s_fail_count = 0;
    s_state = NETLOG_STATE_CONNECTED;
    const uint32_t ip = ntohl(event->ip_info.ip.addr);
    snprintf(s_addr_text, sizeof(s_addr_text), "%u.%u.%u.%u:%u", (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu), (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu), (unsigned)s_port);
    ESP_LOGI(TAG, "netlog up: device=" IPSTR " port=%u", IP2STR(&event->ip_info.ip), (unsigned)s_port);
  }
}

static esp_err_t ensure_wifi_base(void);

/** 一次性装 WiFi 底座：netif、默认事件循环与驱动。步进记账——esp_wifi_init
 *  可能因内存不足失败，重试时已建好的 netif 与事件循环不能重复创建（重复
 *  create 会在 wifi_default 的断言里直接重启）。 */
static esp_err_t ensure_wifi_base(void)
{
  if (!s_netif_ready) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
      return ESP_FAIL;
    }
    s_netif_ready = true;
  }
  if (!s_wifi_ready) {
    if (s_reconnect_timer == NULL) {
      const esp_timer_create_args_t timer_args = { .callback = reconnect_timer_cb, .name = "remapad-reconn" };
      const esp_err_t err = esp_timer_create(&timer_args, &s_reconnect_timer);
      if (err != ESP_OK) {
        return err;
      }
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) {
      return err;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    s_wifi_ready = true;
  }
  return ESP_OK;
}

/** 把已编码的桥接帧发往 PC 目标；目标未学到或未连接时整帧丢弃。 */
static void net_send_frame(const uint8_t *frame, size_t len)
{
  if (s_sock < 0 || !s_dest_set) {
    return;
  }
  const int n = sendto(s_sock, frame, len, 0, (const struct sockaddr *)&s_dest, sizeof(s_dest));
  if (n < 0) {
    s_dropped++;
  } else {
    s_sent++;
  }
}

/** 网络帧回调：升级与 amiibo 上传只走串口（UDP 会丢包，写 flash 的会话不容错），
 *  PING 在这里直接应答，其余交给桥接输入源。 */
static void on_net_frame(const input_frame_view_t *frame, void *user)
{
  (void)user;
  if (frame->version != INPUT_FRAME_VERSION) {
    return;
  }
  s_frames++;
  if (ota_link_is_frame_type(frame->type) || amiibo_session_is_frame_type(frame->type)) {
    static bool warned;
    if (!warned) {
      warned = true;
      ESP_LOGW(TAG, "frame 0x%02x over wifi ignored (serial only)", frame->type);
    }
    return;
  }
  if (frame->type == INPUT_FRAME_TYPE_PING) {
    const uint8_t version = INPUT_FRAME_VERSION;
    uint8_t reply[INPUT_FRAME_MAX_LEN];
    const size_t len = input_frame_encode(reply, sizeof(reply), INPUT_FRAME_TYPE_PING, 0, 0, &version, sizeof(version));
    net_send_frame(reply, len);
    return;
  }
  input_source_handle_frame(frame);
}

static void on_net_text(const uint8_t *text, size_t len, void *user)
{
  (void)user;
  cli_feed_bytes(text, len);
}

/** 收任务常驻：开机就建好（堆还完整时占住 8K 栈），平时停泊，会话开关只翻标志。
 *  8K 深是因为 cli_feed_bytes 在喂入者任务上就地执行命令（amiibo 的 SPIFFS 链最深），
 *  而 esp_wifi_init 一跑就把内部 RAM 最大空闲块切到 8K 以下——晚建就永远建不起来。 */
static uint8_t s_rx_buf[NETLOG_RX_BUF];

/** 收任务：UDP 报文按帧同步字分流（桥接帧 / CLI 文本同一条字节流，与串口同模型）。 */
static void netlog_rx_task(void *param)
{
  (void)param;
  for (;;) {
    if (!s_running || s_sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(NETLOG_RX_POLL_MS));
      continue;
    }
    struct sockaddr_in from = { 0 };
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(s_sock, s_rx_buf, sizeof(s_rx_buf), 0, (struct sockaddr *)&from, &from_len);
    if (n <= 0) {
      continue; /* 收超时或会话已停。 */
    }
    /* 目标自学习：最后说话的接收端拿到日志与帧回流（换接收端即换目标）。 */
    s_dest = from;
    s_dest_set = true;
    s_last_rx_us = esp_timer_get_time();
    input_frame_rx_feed(&s_frame_rx, s_rx_buf, (size_t)n, on_net_frame, on_net_text, NULL);
  }
}

static esp_err_t ensure_rx_task(void)
{
  if (s_rx_task != NULL) {
    return ESP_OK;
  }
  if (xTaskCreate(netlog_rx_task, "remapad-netlog", NETLOG_RX_TASK_STACK, NULL, NETLOG_RX_TASK_PRIO, &s_rx_task) !=
      pdPASS) {
    s_rx_task = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t netlog_start(const char *ssid, const char *password, const char *host, uint16_t port)
{
  if (s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
  snprintf(s_pass, sizeof(s_pass), "%s", password);
  s_port = port == 0 ? NETLOG_PORT_DEFAULT : port;
  memset(&s_dest, 0, sizeof(s_dest));
  s_dest_set = false;
  if (host != NULL && host[0] != '\0') {
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(s_port);
    if (inet_pton(AF_INET, host, &s_dest.sin_addr) != 1) {
      return ESP_ERR_INVALID_ARG;
    }
    s_dest_set = true;
  }
  /* 会话第一行报上次复位原因：崩溃（panic）与掉电（brownout）在远程日志里一眼可分。 */
  ESP_LOGI(TAG, "session start, reset reason=%d", (int)esp_reset_reason());
  /* 收任务先建：要抢在 WiFi 驱动吃掉内部大块之前拿到 8K 连续栈。 */
  const esp_err_t task_err = ensure_rx_task();
  if (task_err != ESP_OK) {
    return task_err;
  }
  /* 顺序要紧：先起 WiFi 底座（esp_netif_init 拉起 lwIP 的 tcpip 任务），再开
     * socket——tcpip 任务没起就调 socket 会在 lwIP 断言里直接重启。 */
  const esp_err_t base_err = ensure_wifi_base();
  if (base_err != ESP_OK) {
    return base_err;
  }
  if (s_sock < 0) {
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
      return ESP_FAIL;
    }
    struct sockaddr_in local = { 0 };
    local.sin_family = AF_INET;
    local.sin_port = htons(s_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
      ESP_LOGW(TAG, "bind :%u failed", (unsigned)s_port);
    }
    struct timeval rx_timeout = { .tv_sec = 0, .tv_usec = NETLOG_RX_POLL_MS * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout));
  }
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t sta = { 0 };
  strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
  strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
  sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  sta.sta.pmf_cfg.capable = false;
  sta.sta.pmf_cfg.required = false;
  esp_wifi_set_config(WIFI_IF_STA, &sta);

  /* 国家码设 CN（信道 1-13，发射功率限幅按国内法规允许的上限走）；功率上限要在
   * start 后另行设置（见 NETLOG_TX_POWER_QUARTER_DBM）。 */
  wifi_country_t country = {
    .cc = "CN",
    .schan = 1,
    .nchan = 13,
    .max_tx_power = 80,
    .policy = WIFI_COUNTRY_POLICY_AUTO,
  };
  esp_wifi_set_country(&country);

  esp_log_level_set("wifi", ESP_LOG_VERBOSE);
  esp_log_level_set("net80211", ESP_LOG_VERBOSE);
  esp_log_level_set("wpa", ESP_LOG_VERBOSE);

  s_sent = 0;
  s_dropped = 0;
  s_frames = 0;
  s_last_rx_us = 0;
  s_fail_count = 0;
  input_frame_rx_reset(&s_frame_rx);
  s_state = NETLOG_STATE_CONNECTING;
  s_running = true;
  /* 日志与 CLI 回复的抄送出口在会话开起来才挂上，会话关掉就摘下。 */
  console_out_set_sink(netlog_sink_write);
  input_link_set_net_tx(net_send_frame, netlog_bridge_active);
  const esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    s_running = false;
    s_state = NETLOG_STATE_OFF;
    input_link_set_net_tx(NULL, NULL);
    return err;
  }
  s_wifi_started = true;
  /* 功率上限要在 start 后设置才生效。 */
  esp_wifi_set_max_tx_power(NETLOG_TX_POWER_QUARTER_DBM);
  /* start 只是起驱动，关联要显式发起；断线重连在 WiFi 事件里补。 */
  esp_wifi_connect();
  ESP_LOGI(TAG, "netlog session ssid=%s port=%u (dest %s)", s_ssid, (unsigned)s_port, s_dest_set ? "set" : "learn");
  return ESP_OK;
}

void netlog_stop(void)
{
  if (!s_running) {
    return;
  }
  s_running = false;
  s_state = NETLOG_STATE_OFF;
  console_out_set_sink(NULL);
  input_link_set_net_tx(NULL, NULL);
  /* 挂起的重连定时器一并撤掉：会话停了就不该再摸 WiFi。 */
  if (s_reconnect_timer != NULL) {
    esp_timer_stop(s_reconnect_timer);
  }
  /* 收任务常驻：socket 一关它就回停泊循环，不需要也不能删。 */
  if (s_sock >= 0) {
    close(s_sock);
    s_sock = -1;
  }
  esp_wifi_disconnect();
  esp_wifi_stop();
  s_wifi_started = false;
  ESP_LOGI(TAG, "netlog session stopped (sent=%lu dropped=%lu)", (unsigned long)s_sent, (unsigned long)s_dropped);
}

esp_err_t netlog_phy_reset(void)
{
  return esp_phy_erase_cal_data_in_nvs();
}

esp_err_t netlog_get_tx_power(int8_t *quarter_dbm)
{
  if (!s_wifi_started) {
    return ESP_ERR_INVALID_STATE;
  }
  return esp_wifi_get_max_tx_power(quarter_dbm);
}

esp_err_t netlog_set_tx_power(int8_t quarter_dbm)
{
  if (!s_wifi_started) {
    return ESP_ERR_INVALID_STATE;
  }
  return esp_wifi_set_max_tx_power(quarter_dbm);
}

esp_err_t netlog_reconnect(void)
{
  if (!s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  /* 断线事件里照常起 3 秒重连定时器，等价于一轮全新的扫描-认证-关联。 */
  return esp_wifi_disconnect();
}

esp_err_t netlog_save_wifi(const char *ssid, const char *password)
{
  /* 只落盘不连接：会话由界面开关或 CLI 手动起，保存不惊动在跑的会话。 */
  return app_config_set_wifi(ssid, password);
}

static const char *authmode_name(wifi_auth_mode_t mode)
{
  switch (mode) {
  case WIFI_AUTH_OPEN:
    return "open";
  case WIFI_AUTH_WEP:
    return "wep";
  case WIFI_AUTH_WPA_PSK:
    return "wpa";
  case WIFI_AUTH_WPA2_PSK:
    return "wpa2";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "wpa/wpa2";
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return "wpa2-ent";
  case WIFI_AUTH_WPA3_PSK:
    return "wpa3";
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "wpa2/wpa3";
  case WIFI_AUTH_WPA3_ENTERPRISE:
    return "wpa3-ent";
  default:
    return "other";
  }
}

esp_err_t netlog_scan(int *count);

bool netlog_scan_line(int index, char *out, size_t cap)
{
  if (index < 0 || index >= s_scan_count) {
    return false;
  }
  const wifi_ap_record_t *record = &s_scan_records[index];
  const uint8_t *mac = record->bssid;
  snprintf(out, cap, "%d: %s " MACSTR " ch%u %ddBm %s", index + 1, (const char *)record->ssid, MAC2STR(mac),
           (unsigned)record->primary, (int)record->rssi, authmode_name(record->authmode));
  return true;
}

static TaskHandle_t s_scan_task;

/** 扫描任务：压住重连、断开、扫一圈、取结果逐条打日志、回连。
 *  同步在调用者上下文里扫会在 esp_wifi_scan_start 一路卡死不返回（CLI 任务就是这么
 *  被吊死的，事件与设备本身都还活着），所以会挂的操作全部挪到这个任务里执行。 */
static void scan_task(void *param)
{
  (void)param;
  const bool resume = s_running;
  if (resume) {
    s_scan_hold = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  wifi_scan_config_t config = { 0 };
  /* 重连状态机收尾（认证交换要几秒才超时）期间扫描起不来：非阻塞发起 + 忙等重试。 */
  esp_err_t scan_err = ESP_OK;
  for (int retry = 0; retry < 10; retry++) {
    scan_err = esp_wifi_scan_start(&config, false);
    if (scan_err != ESP_ERR_WIFI_STATE) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (scan_err == ESP_OK) {
    s_scan_done = false;
    for (int waited = 0; waited < 150 && !s_scan_done; waited++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_scan_done) {
      scan_err = ESP_ERR_TIMEOUT;
    }
  }
  s_scan_count = 0;
  if (scan_err == ESP_OK) {
    /* 先取总数再按缓冲上限截取：直接取记录在发现数大于缓冲时会报 INVALID_SIZE。 */
    uint16_t total = 0;
    if (esp_wifi_scan_get_ap_num(&total) == ESP_OK) {
      uint16_t number = total < NETLOG_SCAN_MAX ? total : NETLOG_SCAN_MAX;
      if (number > 0 && esp_wifi_scan_get_ap_records(&number, s_scan_records) == ESP_OK) {
        s_scan_count = (int)number;
      }
    }
  }
  if (resume) {
    s_scan_hold = false;
    esp_wifi_connect();
  }
  if (scan_err != ESP_OK) {
    ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(scan_err));
  } else {
    for (int i = 0; i < s_scan_count; i++) {
      char line[104];
      if (netlog_scan_line(i, line, sizeof(line))) {
        ESP_LOGI(TAG, "%s", line);
      }
    }
    ESP_LOGI(TAG, "scan done: %d aps (netlog scanlist reprints)", s_scan_count);
  }
  s_scan_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t netlog_scan(int *count)
{
  if (count == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *count = s_scan_count;
  /* 只在 WiFi 驱动初始化成功之后扫：开机头一分钟的内存窗口里抢跑 esp_wifi_init
   * 会在半路失败、deinit 也回不去（驱动就此卡死：能 start 但扫描永不完成），
   * 自启或屏幕开关起过一次会话后再用。 */
  if (!s_wifi_ready) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_scan_task != NULL) {
    return ESP_OK; /* 已在扫：幂等成功，结果稍后见日志。 */
  }
  if (xTaskCreate(scan_task, "remapad-scan", 4096, NULL, 3, &s_scan_task) != pdPASS) {
    s_scan_task = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void netlog_sink_write(const char *text, size_t len)
{
  /* 重入保护：发送路径若再触发日志会无限递归（lwIP 自身的日志就是这种来源）；
   * 目标未自学到之前没处可发，同样静默丢弃。 */
  if (s_sock < 0 || !s_running || !s_dest_set || s_in_sink) {
    return;
  }
  s_in_sink = true;
  const int n = sendto(s_sock, text, len, 0, (const struct sockaddr *)&s_dest, sizeof(s_dest));
  s_in_sink = false;
  if (n < 0) {
    s_dropped++;
  } else {
    s_sent++;
  }
}

bool netlog_running(void)
{
  return s_running;
}

bool netlog_connected(void)
{
  return s_state == NETLOG_STATE_CONNECTED;
}

bool netlog_bridge_active(void)
{
  return s_running && s_dest_set && esp_timer_get_time() - s_last_rx_us < NETLOG_BRIDGE_WINDOW_US;
}

void netlog_addr_text(char *out, size_t cap)
{
  snprintf(out, cap, "%s", s_state == NETLOG_STATE_CONNECTED ? s_addr_text : "--");
}

int netlog_rssi(void)
{
  if (s_state != NETLOG_STATE_CONNECTED) {
    return 0;
  }
  wifi_ap_record_t record;
  if (esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
    return 0;
  }
  return record.rssi;
}

void netlog_status_line(char *out, size_t cap)
{
  char dest[NETLOG_HOST_MAX] = "-";
  if (s_dest_set) {
    const uint32_t ip = ntohl(s_dest.sin_addr.s_addr);
    snprintf(dest, sizeof(dest), "%u.%u.%u.%u:%u", (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu), (unsigned)s_port);
  }
  snprintf(out, cap, "netlog state=%s ssid=%s dest=%s frames=%lu sent=%lu dropped=%lu", state_name(),
           s_running ? s_ssid : "-", dest, (unsigned long)s_frames, (unsigned long)s_sent, (unsigned long)s_dropped);
}

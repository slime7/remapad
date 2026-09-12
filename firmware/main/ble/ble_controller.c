#include "ble_controller.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_session.h"
#include "ns2_frames.h"

static const char *TAG = "remapad_blctl";

/** 设置 host 侧 public 地址；IDF 6.1 的头文件移除了声明但符号仍导出。 */
int ble_hs_id_set_pub(const uint8_t *pub_addr);

/** 真实 Pro Controller 2 的广播地址前缀：Nintendo OUI（2024 年注册，
 * ndeadly/switch2_controller_research 抓包中全部广播包均为该前缀）。 */
static const uint8_t ADV_ADDR_OUI[3] = {0x98, 0xE2, 0x55};

/** Switch 2 主机在芯片层只放行任天堂广播帧，地址 OUI 一并参与过滤；
 * 广播须以 Nintendo OUI 的 public 地址发出。后缀取 eFuse MAC 低 3 字节，
 * 每次上电稳定不变，保证配对凭证与回连地址一致。
 * NimBLE 地址按小端存储（val[0] 为可读序末字节），故整体反转写入。 */
static void set_adv_address(void)
{
    uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const uint8_t addr[6] = {mac[5], mac[4], mac[3],
                             ADV_ADDR_OUI[2], ADV_ADDR_OUI[1], ADV_ADDR_OUI[0]};
    const int rc = ble_hs_id_set_pub(addr);
    if (rc != 0) {
        ESP_LOGE(TAG, "set public adv addr rc=%d", rc);
    }
}

/* ---- GATT UUID 表（controller.md §4）----
 * NimBLE 以空中传输字节序（小端）定义 128 位 UUID，即把规范字符串
 * "xxxxxxxx-...." 连字符去除后整体反转。 */

#define CHR_BASE_STATUS 1
#define CHR_BASE_CONFIG 2
#define CHR_DEVICE_ID 3
#define CHR_INPUT05 4
#define CHR_INPUT09 5
#define CHR_RUMBLE 6
#define CHR_CMD 7
#define CHR_COMPOSITE 8
#define CHR_FWUPG 9
#define CHR_ANSWER 10
#define CHR_ANSWER2 11

static const ble_uuid128_t uuid_svc_vendor =
    BLE_UUID128_INIT(0x80, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t uuid_base_status =
    BLE_UUID128_INIT(0x81, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t uuid_base_config =
    BLE_UUID128_INIT(0x82, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t uuid_device_id =
    BLE_UUID128_INIT(0x83, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t uuid_svc_hid =
    BLE_UUID128_INIT(0xd0, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t uuid_input05 =
    BLE_UUID128_INIT(0xd2, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t uuid_input09 =
    BLE_UUID128_INIT(0xf8, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82,
                     0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74);
static const ble_uuid128_t uuid_rumble =
    BLE_UUID128_INIT(0x05, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9,
                     0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const ble_uuid128_t uuid_cmd =
    BLE_UUID128_INIT(0x05, 0xf0, 0xe5, 0x4f, 0xa5, 0x1e, 0x44, 0xaf,
                     0x6c, 0x4e, 0xb7, 0x8e, 0xc9, 0x4a, 0x9d, 0x64);
static const ble_uuid128_t uuid_composite =
    BLE_UUID128_INIT(0x79, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e,
                     0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);
static const ble_uuid128_t uuid_fwupg =
    BLE_UUID128_INIT(0x8d, 0x9f, 0xf5, 0x5d, 0x3e, 0xd2, 0xf7, 0xa4,
                     0xf7, 0x4d, 0xae, 0xfd, 0x3d, 0x42, 0x47, 0x41);
static const ble_uuid128_t uuid_answer =
    BLE_UUID128_INIT(0x6a, 0x83, 0x11, 0xb1, 0x15, 0x53, 0x0a, 0xa2,
                     0x36, 0x4d, 0xd8, 0xd9, 0x61, 0xa9, 0x65, 0xc7);
static const ble_uuid128_t uuid_answer2 =
    BLE_UUID128_INIT(0xe0, 0x57, 0x76, 0xa7, 0x6b, 0x32, 0x49, 0xa5,
                     0x95, 0x4e, 0x78, 0x42, 0x7d, 0x9f, 0x6d, 0x50);
static const ble_uuid128_t uuid_report_rate =
    BLE_UUID128_INIT(0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95,
                     0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55, 0x9d, 0x67);

/* 真实手柄在 0x0010 / 0x001C 处存在文档未列出的描述符；用报告率描述符
 * 同 UUID 占位，使 rumble/answer 等关键特征值句柄与 controller.md §4 对齐。 */

static struct {
    uint16_t input05;
    uint16_t input09;
    uint16_t answer;
} s_h;

static struct {
    bool connected;
    uint16_t conn_handle;
    bool input05_notify;
    bool input09_notify;
    bool answer_notify;
    uint8_t last_input05[63];
    uint8_t last_input09[63];
} s_ctl;

void ble_store_config_init(void);

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_vendor.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &uuid_base_status.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ, .arg = (void *)CHR_BASE_STATUS},
            {.uuid = &uuid_base_config.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE, .arg = (void *)CHR_BASE_CONFIG},
            {.uuid = &uuid_device_id.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ, .arg = (void *)CHR_DEVICE_ID},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_hid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &uuid_input05.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .arg = (void *)CHR_INPUT05,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_input09.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .arg = (void *)CHR_INPUT09,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_rumble.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_RUMBLE},
            {.uuid = &uuid_cmd.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_CMD},
            {.uuid = &uuid_composite.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_COMPOSITE},
            {.uuid = &uuid_fwupg.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_FWUPG},
            {.uuid = &uuid_answer.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_NOTIFY, .arg = (void *)CHR_ANSWER,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_answer2.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_NOTIFY, .arg = (void *)CHR_ANSWER2},
            {0},
        },
    },
    {0},
};

static int read_flat(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uintptr_t tag = (uintptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (tag) {
        case CHR_INPUT05:
            return read_flat(ctxt, s_ctl.last_input05, sizeof(s_ctl.last_input05));
        case CHR_INPUT09:
            return read_flat(ctxt, s_ctl.last_input09, sizeof(s_ctl.last_input09));
        default: {
            /* 厂商基础状态/设备标识的字段语义文档未给出，返回 0 填充。 */
            static const uint8_t zeros[4] = {0};
            return read_flat(ctxt, zeros, sizeof(zeros));
        }
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const uint8_t rate_zero[1] = {0};
        return read_flat(ctxt, rate_zero, sizeof(rate_zero));
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[128];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    switch (tag) {
    case CHR_RUMBLE:
        ns2_session_on_output(buf, len);
        break;
    case CHR_CMD:
        ns2_session_on_command(buf, len, NS2_FRAME_TRANSPORT_BLE);
        break;
    case CHR_COMPOSITE:
        ns2_session_on_composite(buf, len);
        break;
    case CHR_FWUPG:
        ESP_LOGW(TAG, "fw upgrade write %uB ignored", len);
        break;
    case CHR_BASE_CONFIG:
        ESP_LOGI(TAG, "vendor base config write %uB", len);
        break;
    default:
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    return 0;
}

/** 注册回调：捕获关键特征值句柄并打印整表，供与 controller.md §4 句柄比对。 */
static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        if (ble_uuid_cmp(ctxt->chr.chr_def->uuid, &uuid_input05.u) == 0) {
            s_h.input05 = ctxt->chr.val_handle;
        } else if (ble_uuid_cmp(ctxt->chr.chr_def->uuid, &uuid_input09.u) == 0) {
            s_h.input09 = ctxt->chr.val_handle;
        } else if (ble_uuid_cmp(ctxt->chr.chr_def->uuid, &uuid_answer.u) == 0) {
            s_h.answer = ctxt->chr.val_handle;
        }
        ESP_LOGI(TAG, "GATT chr 0x%04x (val)", ctxt->chr.val_handle);
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_DSC) {
        ESP_LOGI(TAG, "GATT dsc 0x%04x", ctxt->dsc.handle);
    }
}

static void request_conn_params(uint16_t conn_handle)
{
    /* §11：连接间隔需收敛在 5-10ms；主机为主时以我方偏好发起更新。 */
    const struct ble_gap_upd_params upd = {
        .itvl_min = 4,  /* 5ms */
        .itvl_max = 8,  /* 10ms */
        .latency = 0,
        .supervision_timeout = 500, /* 5s */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    const int rc = ble_gap_update_params(conn_handle, &upd);
    if (rc != 0) {
        ESP_LOGD(TAG, "conn param update rc=%d", rc);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ctl.connected = true;
            s_ctl.conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "ACL connected (conn=%u)", event->connect.conn_handle);
            ns2_session_on_connect(event->connect.conn_handle);
            request_conn_params(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed rc=%d, restart adv", event->connect.status);
            ns2_session_on_disconnect();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_ctl.connected = false;
        s_ctl.conn_handle = 0;
        s_ctl.input05_notify = false;
        s_ctl.input09_notify = false;
        s_ctl.answer_notify = false;
        ESP_LOGI(TAG, "disconnected reason=0x%02x", event->disconnect.reason);
        ns2_session_on_disconnect();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "adv complete, restart");
        ns2_session_on_disconnect();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_h.input05) {
            s_ctl.input05_notify = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_h.input09) {
            s_ctl.input09_notify = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_h.answer) {
            s_ctl.answer_notify = event->subscribe.cur_notify != 0;
        }
        ESP_LOGI(TAG, "subscribe 0x%04x notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU -> %u", event->mtu.value);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* NS2 禁用标准 SMP（controller.md §3）：忽略重复配对请求。 */
        ESP_LOGW(TAG, "repeat pairing ignored");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}

void ble_controller_advertise(const uint8_t payload[31])
{
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    const int rc_set = ble_gap_adv_set_data(payload, 31);
    if (rc_set != 0) {
        ESP_LOGE(TAG, "adv set data rc=%d", rc_set);
        return;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x20; /* 32 x 0.625ms = 20ms */
    params.itvl_max = 0x30; /* 37.5ms */
    const int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                                     &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    }
}

static void notify(uint16_t attr_handle, bool enabled, const uint8_t *data, size_t len)
{
    if (!s_ctl.connected || !enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return;
    }
    ble_gatts_notify_custom(s_ctl.conn_handle, attr_handle, om);
}

void ble_controller_notify_input_05(const uint8_t report[63])
{
    memcpy(s_ctl.last_input05, report, 63);
    notify(s_h.input05, s_ctl.input05_notify, report, 63);
}

void ble_controller_notify_input_09(const uint8_t report[63])
{
    memcpy(s_ctl.last_input09, report, 63);
    notify(s_h.input09, s_ctl.input09_notify, report, 63);
}

void ble_controller_notify_answer(const uint8_t *frame, size_t len)
{
    if (len > 96) {
        return;
    }
    notify(s_h.answer, s_ctl.answer_notify, frame, len);
}

bool ble_controller_connected(void)
{
    return s_ctl.connected;
}

bool ble_controller_disconnect(void)
{
    if (!s_ctl.connected) {
        return false;
    }
    const int rc = ble_gap_terminate(s_ctl.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGW(TAG, "gap terminate rc=%d", rc);
        return false;
    }
    ESP_LOGI(TAG, "terminate initiated (conn=%u)", s_ctl.conn_handle);
    return true;
}

bool ble_controller_peer_mac(uint16_t conn_handle, uint8_t out_mac[6])
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return false;
    }
    memcpy(out_mac, desc.peer_id_addr.val, 6);
    return true;
}

bool ble_controller_input_notify_ready(uint8_t report_format)
{
    if (report_format == 5) {
        return s_ctl.input05_notify;
    }
    return s_ctl.input09_notify;
}

static void on_sync(void)
{
    /* 须在 ensure_addr 之前设置：若此时 host 侧仍无地址，ensure_addr 会
     * 读取并采用 controller 的 Espressif 地址，伪装即被覆盖。 */
    set_adv_address();
    const int rc = ble_hs_util_ensure_addr(BLE_ADDR_PUBLIC);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr rc=%d", rc);
        return;
    }
    uint8_t own_mac[6];
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, own_mac, NULL);
    ns2_session_on_sync(own_mac);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    /* 阻塞运行 NimBLE host 事件循环，直到 nimble_port_stop()。 */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_controller_start(void)
{
    memset(&s_h, 0, sizeof(s_h));
    memset(&s_ctl, 0, sizeof(s_ctl));

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add rc=%d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

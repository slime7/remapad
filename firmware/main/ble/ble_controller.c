#include "ble_controller.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

#include "ble_session.h"
#include "ns2_frames.h"

static const char *TAG = "remapad_blctl";

/* ---- GATT UUID 表（真机布局，对齐已验证实现；NimBLE 以空中字节序
 * （小端）定义 128 位 UUID）---- */

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
#define CHR_EXT22 12
#define CHR_EXT26 13
#define CHR_EXT2A 14
#define CHR_EXT2C 15
#define CHR_EXT2E 16
#define CHR_EXT32 17

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
static const ble_uuid128_t uuid_ext22 =
    BLE_UUID128_INIT(0x80, 0x2a, 0x6d, 0x40, 0x6f, 0xf8, 0x15, 0xab,
                     0x41, 0x42, 0x1c, 0x84, 0xd2, 0x69, 0xbd, 0xd3);
static const ble_uuid128_t uuid_ext26 =
    BLE_UUID128_INIT(0xde, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t uuid_ext2a =
    BLE_UUID128_INIT(0xdf, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t uuid_ext2c =
    BLE_UUID128_INIT(0x06, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9,
                     0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const ble_uuid128_t uuid_ext2e =
    BLE_UUID128_INIT(0xf9, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t uuid_ext32 =
    BLE_UUID128_INIT(0x80, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e,
                     0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);
/* 描述符两族：报告率族 679d5510（0x000c/0x0010/0x0028/0x0030）、
 * 通用族 b746df8c（0x001c/0x0020/0x0024）；真机均接受主机写入。 */
static const ble_uuid128_t uuid_report_rate =
    BLE_UUID128_INIT(0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95,
                     0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55, 0x9d, 0x67);
static const ble_uuid128_t uuid_generic_dsc =
    BLE_UUID128_INIT(0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c,
                     0x5b, 0x49, 0x58, 0xf3, 0x8c, 0xdf, 0x46, 0xb7);

static struct {
    uint16_t input05;
    uint16_t input09;
    uint16_t answer;
    uint16_t answer2;
} s_h;

/** 并发连接槽：JoyCon 组合模式下左右两只同时在线。 */
#define BLE_CTL_CONN_MAX 2

/** 广播实例：0 走 BLE5 扩展 PDU、1 走 legacy PDU（Pro 单身份双形态兼容
 * 新旧主机）；JoyCon 双身份各占一个实例（均 legacy PDU，静态随机地址）。 */
#define ADV_INSTANCE_EXT 0
#define ADV_INSTANCE_LEGACY 1
#define ADV_INSTANCE_MAX 2

typedef struct {
    bool used;
    uint16_t conn_handle;
    uint8_t identity; /* ns2_identity_t */
    bool input05_notify;
    bool input09_notify;
    bool answer_notify;
    bool answer2_notify;
    uint8_t last_input05[63];
    uint8_t last_input09[63];
} conn_slot_t;

static conn_slot_t s_conn[BLE_CTL_CONN_MAX];

/** 每实例广播身份与地址：接收连接时按本机地址反查身份；JoyCon 双身份
 * 各占一个实例（静态随机地址），Pro 单身份两实例共用公共伪装地址。 */
static uint8_t s_adv_identity[ADV_INSTANCE_MAX];
static uint8_t s_adv_addr[ADV_INSTANCE_MAX][6];
static bool s_adv_addr_valid[ADV_INSTANCE_MAX];
static uint8_t s_own_public[6];

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
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_input09.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .arg = (void *)CHR_INPUT09,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
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
                 {.uuid = &uuid_generic_dsc.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_answer2.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_NOTIFY, .arg = (void *)CHR_ANSWER2,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_generic_dsc.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_ext22.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_NOTIFY, .arg = (void *)CHR_EXT22,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_generic_dsc.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_ext26.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .arg = (void *)CHR_EXT26,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_ext2a.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_EXT2A},
            {.uuid = &uuid_ext2c.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_EXT2C},
            {.uuid = &uuid_ext2e.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .arg = (void *)CHR_EXT2E,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = &uuid_report_rate.u, .access_cb = chr_access,
                  .att_flags = BLE_ATT_F_READ, .arg = NULL},
                 {0},
             }},
            {.uuid = &uuid_ext32.u, .access_cb = chr_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .arg = (void *)CHR_EXT32},
            {0},
        },
    },
    {0},
};

static int read_flat(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static conn_slot_t *conn_slot(uint16_t conn_handle)
{
    for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
        if (s_conn[i].used && s_conn[i].conn_handle == conn_handle) {
            return &s_conn[i];
        }
    }
    return NULL;
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uintptr_t tag = (uintptr_t)arg;
    conn_slot_t *slot = conn_slot(conn_handle);

    /* 任意 ATT 访问都算主机活动，刷新所在连接的空闲计时。 */
    ns2_session_touch(conn_handle);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (tag) {
        case CHR_INPUT05:
            return read_flat(ctxt, slot ? slot->last_input05 : (uint8_t[63]){0}, 63);
        case CHR_INPUT09:
            return read_flat(ctxt, slot ? slot->last_input09 : (uint8_t[63]){0}, 63);
        case CHR_BASE_STATUS: {
            /* 真机读值（已验证实现基线）。 */
            static const uint8_t base_status[7] = {0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00};
            return read_flat(ctxt, base_status, sizeof(base_status));
        }
        case CHR_DEVICE_ID: {
            /* 真机读值（8B，语义未知）。 */
            static const uint8_t device_id[8] = {
                0x36, 0x80, 0x74, 0xee, 0xbb, 0x3d, 0x8e, 0x13,
            };
            return read_flat(ctxt, device_id, sizeof(device_id));
        }
        default: {
            /* 厂商基础状态/设备标识之外未知特征值返回 0 填充。 */
            static const uint8_t zeros[4] = {0};
            return read_flat(ctxt, zeros, sizeof(zeros));
        }
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const uint8_t rate_zero[1] = {0};
        return read_flat(ctxt, rate_zero, sizeof(rate_zero));
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
        /* 主机初始化时向 0x0010 等报告率描述符写入配置；接受即认可。 */
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[128];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    switch (tag) {
    case CHR_RUMBLE:
        ns2_session_on_output(buf, len, conn_handle);
        break;
    case CHR_CMD:
        ns2_session_on_command(buf, len, NS2_FRAME_TRANSPORT_BLE, conn_handle);
        break;
    case CHR_COMPOSITE:
        ns2_session_on_composite(buf, len, conn_handle);
        break;
    case CHR_FWUPG:
        /* 固件升级数据块（0x0018 WRITE NO RSP）：交给会话的假升级会话。 */
        ns2_session_on_fw_upgrade(buf, len);
        break;
    case CHR_BASE_CONFIG:
        ESP_LOGI(TAG, "vendor base config write %uB", len);
        break;
    case CHR_EXT22:
    case CHR_EXT26:
    case CHR_EXT2A:
    case CHR_EXT2C:
    case CHR_EXT2E:
    case CHR_EXT32:
        /* 未知功能特征值（0x0022-0x0032 段）：接受写入即认可。 */
        ESP_LOGI(TAG, "ext chr write %uB (tag %u)", len, (unsigned)tag);
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
        } else if (ble_uuid_cmp(ctxt->chr.chr_def->uuid, &uuid_answer2.u) == 0) {
            s_h.answer2 = ctxt->chr.val_handle;
        }
        ESP_LOGI(TAG, "GATT chr 0x%04x (val)", ctxt->chr.val_handle);
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_DSC) {
        ESP_LOGI(TAG, "GATT dsc 0x%04x", ctxt->dsc.handle);
    }
}

static void request_conn_params(uint16_t conn_handle)
{
    /* 对齐已验证实现：最小间隔请求 7.5ms（6 单位），上限与超时取主机当前
     * 值（不干扰主机自己的时序），延迟 0。 */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return;
    }
    const struct ble_gap_upd_params upd = {
        .itvl_min = 6, /* 7.5ms */
        .itvl_max = desc.conn_itvl,
        .latency = 0,
        .supervision_timeout = desc.supervision_timeout,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    const int rc = ble_gap_update_params(conn_handle, &upd);
    if (rc != 0) {
        ESP_LOGD(TAG, "conn param update rc=%d", rc);
    }
}

/** 按本机地址反查广播实例（连接落在哪个广播上）；Pro 模式两实例同址，
 * 返回首个命中。未命中返回 -1。 */
static int adv_instance_by_addr(const uint8_t addr[6])
{
    for (size_t i = 0; i < ADV_INSTANCE_MAX; i++) {
        if (s_adv_addr_valid[i] && memcmp(s_adv_addr[i], addr, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** 接收连接的实例是否需要继续广播：JoyCon 双身份下另一只还在等回连，
 * 只停接收实例；Pro 单身份停全部。 */
static void stop_advertising_for(uint8_t identity)
{
    for (size_t i = 0; i < ADV_INSTANCE_MAX; i++) {
        if (s_adv_identity[i] == identity || identity == NS2_ID_PRO) {
            ble_gap_ext_adv_stop((uint8_t)i);
        }
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed rc=%d, resume via session", event->connect.status);
            ns2_session_on_connect_fail();
            break;
        }
        conn_slot_t *slot = NULL;
        for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
            if (!s_conn[i].used) {
                slot = &s_conn[i];
                break;
            }
        }
        if (slot == NULL) {
            ESP_LOGW(TAG, "no free conn slot, terminating %u", event->connect.conn_handle);
            ble_gap_terminate(event->connect.conn_handle, BLE_CTL_DISCONNECT_CONN_FAIL);
            break;
        }
        struct ble_gap_conn_desc desc;
        uint8_t identity = NS2_ID_PRO;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            const int inst = adv_instance_by_addr(desc.our_ota_addr.val);
            if (inst >= 0) {
                identity = s_adv_identity[inst];
                stop_advertising_for(identity);
            } else {
                /* 双实例同址（Pro）：任一连接后都无需继续广播。 */
                ble_controller_adv_stop();
            }
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->conn_handle = event->connect.conn_handle;
        slot->identity = identity;
        ESP_LOGI(TAG, "ACL connected (conn=%u, identity=%u)",
                 event->connect.conn_handle, (unsigned)identity);
        ns2_session_on_connect(event->connect.conn_handle, identity);
        request_conn_params(event->connect.conn_handle);
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        conn_slot_t *slot = conn_slot(event->disconnect.conn.conn_handle);
        const uint8_t identity = slot ? slot->identity : NS2_ID_PRO;
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        ESP_LOGI(TAG, "disconnected reason=0x%02x (identity=%u)",
                 event->disconnect.reason, (unsigned)identity);
        ns2_session_on_disconnect(event->disconnect.conn.conn_handle, identity);
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* 广播超时/连接占用在会话层按身份恢复（本工程广播不限时长，
         * 此事件基本只在连接建立后出现，会话的断连路径已覆盖）。 */
        ESP_LOGD(TAG, "adv complete");
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        conn_slot_t *slot = conn_slot(event->subscribe.conn_handle);
        ns2_session_touch(event->subscribe.conn_handle);
        if (slot != NULL) {
            if (event->subscribe.attr_handle == s_h.input05) {
                slot->input05_notify = event->subscribe.cur_notify != 0;
            } else if (event->subscribe.attr_handle == s_h.input09) {
                slot->input09_notify = event->subscribe.cur_notify != 0;
            } else if (event->subscribe.attr_handle == s_h.answer) {
                slot->answer_notify = event->subscribe.cur_notify != 0;
            } else if (event->subscribe.attr_handle == s_h.answer2) {
                slot->answer2_notify = event->subscribe.cur_notify != 0;
            }
        }
        ESP_LOGI(TAG, "subscribe 0x%04x notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    }
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

/** 启动一个广播实例。addr 非 NULL 时以静态随机地址广播（JoyCon 双身份）。 */
static void adv_start_instance(uint8_t instance, int legacy_pdu, const uint8_t payload[31],
                               const uint8_t addr[6])
{
    if (instance >= ADV_INSTANCE_MAX) {
        return;
    }
    if (ble_gap_ext_adv_active(instance)) {
        ble_gap_ext_adv_stop(instance);
    }
    struct ble_gap_ext_adv_params params = {0};
    /* NS2 主机的芯片层过滤只认经扩展广播 HCI 路径下发的广播：已验证可被
     * 发现的开源实现与真机抓包均为 30ms 间隔。扩展 PDU 按规范不可同时
     * connectable 与 scannable，扩展实例只做可连接广播。 */
    params.legacy_pdu = legacy_pdu;
    params.connectable = 1;
    params.scannable = legacy_pdu;
    params.own_addr_type = addr != NULL ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.itvl_min = 0x30; /* 48 x 0.625ms = 30ms */
    params.itvl_max = 0x30;
    params.sid = 0;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    const int rc_conf = ble_gap_ext_adv_configure(instance, &params, NULL,
                                                  gap_event_cb, NULL);
    if (rc_conf != 0) {
        /* 实例尚未停稳（如连接建立窗口）时 configure 返回忙，随后事件
         * 路径会再次恢复广播，这里只需提示。 */
        ESP_LOGW(TAG, "adv %u configure rc=%d", instance, rc_conf);
        return;
    }
    if (addr != NULL) {
        ble_addr_t random_addr = {.type = BLE_ADDR_RANDOM};
        memcpy(random_addr.val, addr, 6);
        const int rc_addr = ble_gap_ext_adv_set_addr(instance, &random_addr);
        if (rc_addr != 0) {
            ESP_LOGW(TAG, "adv %u set addr rc=%d", instance, rc_addr);
            return;
        }
        memcpy(s_adv_addr[instance], addr, 6);
        s_adv_addr_valid[instance] = true;
    } else {
        memcpy(s_adv_addr[instance], s_own_public, 6);
        s_adv_addr_valid[instance] = true;
    }
    struct os_mbuf *om = os_msys_get_pkthdr(31, 0);
    if (om == NULL) {
        ESP_LOGE(TAG, "adv mbuf alloc failed");
        return;
    }
    if (os_mbuf_append(om, payload, 31) != 0) {
        os_mbuf_free_chain(om);
        ESP_LOGE(TAG, "adv mbuf append failed");
        return;
    }
    const int rc_set = ble_gap_ext_adv_set_data(instance, om);
    if (rc_set != 0) {
        ESP_LOGE(TAG, "adv %u set data rc=%d", instance, rc_set);
        return;
    }
    if (legacy_pdu) {
        struct os_mbuf *rsp = os_msys_get_pkthdr(0, 0);
        if (rsp == NULL || ble_gap_ext_adv_rsp_set_data(instance, rsp) != 0) {
            os_mbuf_free_chain(rsp);
            ESP_LOGW(TAG, "adv %u empty rsp rejected", instance);
        }
    }
    const int rc = ble_gap_ext_adv_start(instance, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv %u start rc=%d", instance, rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started (instance=%u, %s pdu, identity=%u)", instance,
             legacy_pdu ? "legacy" : "extended", (unsigned)s_adv_identity[instance]);
}

void ble_controller_adv_start(uint8_t instance, const uint8_t payload[31],
                              const uint8_t addr[6])
{
    if (instance >= ADV_INSTANCE_MAX) {
        return;
    }
    /* 实例身份由会话层随载荷一并告知：Pro 单身份（双实例同址），JoyCon
     * 双身份各占一实例。addr 为 NULL 时沿用公共伪装地址。 */
    if (addr != NULL) {
        s_adv_identity[instance] =
            (addr[5] & 0xC0) == 0xC0 && (addr[0] & 0x01) != 0 ? NS2_ID_JOYCON_R
                                                              : NS2_ID_JOYCON_L;
    } else {
        s_adv_identity[instance] = NS2_ID_PRO;
    }
    ESP_LOG_BUFFER_HEX(TAG, payload, 31);
    adv_start_instance(instance, addr != NULL ? 1 : (instance == ADV_INSTANCE_LEGACY),
                       payload, addr);
}

void ble_controller_adv_stop(void)
{
    for (uint8_t i = 0; i < ADV_INSTANCE_MAX; i++) {
        ble_gap_ext_adv_stop(i);
    }
}

static void notify(uint16_t conn_handle, uint16_t attr_handle, bool enabled,
                   const uint8_t *data, size_t len)
{
    if (!enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return;
    }
    ble_gatts_notify_custom(conn_handle, attr_handle, om);
}

bool ble_controller_connected(void)
{
    for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
        if (s_conn[i].used) {
            return true;
        }
    }
    return false;
}

size_t ble_controller_conn_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
        if (s_conn[i].used) {
            n++;
        }
    }
    return n;
}

void ble_controller_notify_input_05(uint16_t conn_handle, const uint8_t report[63])
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return;
    }
    memcpy(slot->last_input05, report, 63);
    notify(conn_handle, s_h.input05, slot->input05_notify, report, 63);
}

void ble_controller_notify_input_09(uint16_t conn_handle, const uint8_t report[63])
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return;
    }
    memcpy(slot->last_input09, report, 63);
    notify(conn_handle, s_h.input09, slot->input09_notify, report, 63);
}

void ble_controller_notify_answer(uint16_t conn_handle, const uint8_t *frame, size_t len)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL || len > 160) {
        return;
    }
    /* 上限对齐 ns2_session_on_command 的应答缓冲（14B 前缀 + 8B 帧头 +
     * 0x78 最大读取体）。 */
    notify(conn_handle, s_h.answer2, slot->answer2_notify, frame, len);
}

/** 周期检查连接空闲：握手未完成且超时无协议活动的主机（手机/PC 回连）
 * 主动断开，释放广播；主机初始化序列毫秒级到达，不受影响。 */
static void host_idle_timer_cb(void *arg)
{
    for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
        if (s_conn[i].used && ns2_session_conn_idle_expired(s_conn[i].conn_handle)) {
            ESP_LOGW(TAG, "host idle timeout, disconnecting conn=%u",
                     s_conn[i].conn_handle);
            ble_gap_terminate(s_conn[i].conn_handle, BLE_CTL_DISCONNECT_USER_TERM);
        }
    }
    ns2_session_tick();
}

void ble_controller_disconnect(uint8_t hci_reason)
{
    for (size_t i = 0; i < BLE_CTL_CONN_MAX; i++) {
        if (s_conn[i].used) {
            const int rc = ble_gap_terminate(s_conn[i].conn_handle, hci_reason);
            if (rc != 0) {
                ESP_LOGW(TAG, "gap terminate rc=%d", rc);
            } else {
                ESP_LOGI(TAG, "terminate initiated (conn=%u)", s_conn[i].conn_handle);
            }
        }
    }
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

bool ble_controller_input_notify_ready(uint16_t conn_handle, uint8_t report_format)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return false;
    }
    return report_format == 5 ? slot->input05_notify : slot->input09_notify;
}

bool ble_controller_conn_identity(uint16_t conn_handle, uint8_t *identity)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return false;
    }
    *identity = slot->identity;
    return true;
}

static void on_sync(void)
{
    /* base MAC 已在 app_main 换为 Nintendo OUI，controller 读回的 public
     * 地址即伪装地址，无需 host 侧干预。 */
    const int rc = ble_hs_util_ensure_addr(BLE_ADDR_PUBLIC);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr rc=%d", rc);
        return;
    }
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, s_own_public, NULL);
    /* 对齐已验证实现：声明更大的首选 MTU，连接后偏好 2M PHY。 */
    ble_att_set_preferred_mtu(512);
    ble_gap_set_prefered_default_le_phy(BLE_HCI_LE_PHY_2M_PREF_MASK,
                                        BLE_HCI_LE_PHY_2M_PREF_MASK);
    ns2_session_on_sync(s_own_public);
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
    memset(s_conn, 0, sizeof(s_conn));
    memset(s_adv_identity, 0, sizeof(s_adv_identity));
    memset(s_adv_addr, 0, sizeof(s_adv_addr));
    memset(s_adv_addr_valid, 0, sizeof(s_adv_addr_valid));

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    /* 主机流程含标准 SMP 配对（pair 不 bond，LTK 源头仍是 0x15 私有配对），
     * 配置对齐已验证可完成配对的开源实现；store 用默认 RAM bonding 库。 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_oob_data_flag = 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
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
    esp_timer_handle_t idle_timer = NULL;
    const esp_timer_create_args_t idle_timer_args = {
        .callback = host_idle_timer_cb,
        .name = "ble_idle_chk",
    };
    if (esp_timer_create(&idle_timer_args, &idle_timer) == ESP_OK) {
        esp_timer_start_periodic(idle_timer, 1000000LL);
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

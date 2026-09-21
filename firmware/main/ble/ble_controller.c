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
#include "dp_capture.h"
#include "ns2_identity.h"
#include "ns2_frames.h"

static const char *TAG = "remapad_blctl";

/* ---- GATT UUID 表（对齐已验证实现；NimBLE 以空中字节序
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
 * 通用族 b746df8c（0x001c/0x0020/0x0024）；两族都接受主机写入。 */
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

/** 并发连接槽：一台主机一条链路；第二槽给主机换地址重连等过渡情形留余量。 */
#define BLE_CTL_CONN_MAX 2

/** 广播实例：设备只发一台 Pro 的广播（默认 legacy PDU，见
 *  ble_controller_adv_start）；两个实例是历史遗留的接法余量。 */
#define ADV_INSTANCE_EXT 0
#define ADV_INSTANCE_LEGACY 1
#define ADV_INSTANCE_MAX 2

typedef struct {
    bool used;
    uint16_t conn_handle;
    uint8_t identity; /* ns2_identity_t */
    bool input05_notify;
    /** 专用输入通道（0x07 / 0x08 / 0x09）的订阅状态与句柄：主机在同一个
     *  0x000E 句柄上按型号换 UUID，本设备把三种都注册出来，主机订哪一个
     *  就往哪一个发通知。 */
    bool input_priv_notify;
    uint16_t input_priv_handle;
    bool answer_notify;
    bool answer2_notify;
    uint16_t conn_itvl; /* 1.25ms 单位 */
    uint16_t mtu;       /* 协商后的 ATT MTU（63B 通知需要 >= 66） */
    uint32_t tx_fail;   /* 通知投递失败计数（订阅成功但主机收不到输入的判据） */
    int tx_rc;          /* 最近一次失败的返回码 */
    uint8_t last_input05[63];
    /** 专用输入通道的快照（按会话格式是 0x07 / 0x08 / 0x09 之一）：主机不订阅
     *  而改用 READ 轮询时读到的就是这份值。 */
    uint8_t last_input_priv[63];
    bool input_read_logged; /* 主机是否 READ 过输入通道（只留首次一条日志） */
} conn_slot_t;

static conn_slot_t s_conn[BLE_CTL_CONN_MAX];

/** 每实例广播身份与地址：接收连接时按本机地址反查身份；Pro 单身份两实例
 *  共用公共伪装地址。 */
static uint8_t s_adv_identity[ADV_INSTANCE_MAX];
static uint8_t s_adv_addr[ADV_INSTANCE_MAX][6];
static bool s_adv_addr_valid[ADV_INSTANCE_MAX];
static uint8_t s_own_public[6];

/** 广播 PDU 形态（对账开关，默认按实例默认）：见 ble_ctl_adv_pdu_form_t。 */
static uint8_t s_adv_pdu_form;

const char *ble_controller_adv_pdu_form_name(uint8_t form)
{
    switch (form) {
    case BLE_CTL_ADV_PDU_LEGACY:
        return "legacy";
    case BLE_CTL_ADV_PDU_EXTENDED:
        return "extended";
    default:
        return "auto";
    }
}

void ble_controller_set_adv_pdu_form(uint8_t form)
{
    if (form > BLE_CTL_ADV_PDU_EXTENDED) {
        return;
    }
    s_adv_pdu_form = form;
    ESP_LOGW(TAG, "adv pdu form -> %s", ble_controller_adv_pdu_form_name(form));
}

uint8_t ble_controller_adv_pdu_form(void)
{
    return s_adv_pdu_form;
}

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

/** 未知特征值写入留痕：长度 + 前 8 字节，只作对账（协议未定，不参与业务）。 */
static void log_write_head(const char *what, uint16_t tag, const struct os_mbuf *om)
{
    const size_t total = OS_MBUF_PKTLEN(om);
    uint8_t head[8] = {0};
    const size_t n = total < sizeof(head) ? total : sizeof(head);
    if (n > 0) {
        os_mbuf_copydata(om, 0, (int)n, head);
    }
    ESP_LOGI(TAG, "%s %uB (tag %u) %02x %02x %02x %02x %02x %02x %02x %02x", what,
             (unsigned)total, (unsigned)tag, head[0], head[1], head[2], head[3],
             head[4], head[5], head[6], head[7]);
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

/** 是否为输入通道（0x05 通用 / 0x09 专用）。 */
static bool is_input_chr(uintptr_t tag)
{
    return tag == CHR_INPUT05 || tag == CHR_INPUT09;
}

/** 写入特征值 → 采集通道字节（GATT 属性表的句柄低字节，
 *  PC 侧按同一张表还原通道名）。 */
static uint8_t capture_channel(uintptr_t tag)
{
    switch (tag) {
    case CHR_BASE_CONFIG:
        return DP_CAPTURE_CH_BASE_CONFIG;
    case CHR_RUMBLE:
        return DP_CAPTURE_CH_RUMBLE;
    case CHR_CMD:
        return DP_CAPTURE_CH_CMD;
    case CHR_COMPOSITE:
        return DP_CAPTURE_CH_COMPOSITE;
    case CHR_FWUPG:
        return DP_CAPTURE_CH_FWUPG;
    case CHR_EXT22:
        return DP_CAPTURE_CH_EXT22;
    case CHR_EXT26:
        return DP_CAPTURE_CH_EXT26;
    case CHR_EXT2A:
        return DP_CAPTURE_CH_EXT2A;
    case CHR_EXT2C:
        return DP_CAPTURE_CH_EXT2C;
    case CHR_EXT2E:
        return DP_CAPTURE_CH_EXT2E;
    case CHR_EXT32:
        return DP_CAPTURE_CH_EXT32;
    default:
        return 0;
    }
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uintptr_t tag = (uintptr_t)arg;
    conn_slot_t *slot = conn_slot(conn_handle);

    /* 任意 ATT 访问都算主机活动，刷新所在连接的空闲计时。 */
    ns2_session_touch(conn_handle);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* 输入通道之外的特征值读取（含未实现特征值）留痕：主机在升级等流程
         * 里若读某个状态位，日志里能看出它期待什么。 */
        if (!is_input_chr(tag)) {
            ESP_LOGI(TAG, "chr read tag=%u (handle=0x%04x)", (unsigned)tag, attr_handle);
        } else if (slot != NULL && !slot->input_read_logged) {
            /* 主机没订阅输入通道、改用 READ 轮询取输入时的现场证据。 */
            slot->input_read_logged = true;
            ESP_LOGI(TAG, "input poll read (handle=0x%04x, tag=%u)", attr_handle,
                     (unsigned)tag);
        }
        switch (tag) {
        case CHR_INPUT05:
            return read_flat(ctxt, slot ? slot->last_input05 : (uint8_t[63]){0}, 63);
        case CHR_INPUT09:
            return read_flat(ctxt, slot ? slot->last_input_priv : (uint8_t[63]){0}, 63);
        case CHR_BASE_STATUS: {
            /* 基线读值（已验证实现）。 */
            static const uint8_t base_status[7] = {0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00};
            return read_flat(ctxt, base_status, sizeof(base_status));
        }
        case CHR_DEVICE_ID: {
            /* 读值（8B，语义未知）。 */
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
        /* 主机初始化末尾会向 0x000C/0x0010 写报告率描述符（`85 00`）。
         * 写到这里说明主机的初始化序列已走到订阅输入前一步，日志留痕便于
         * 判断握手停在哪一步。 */
        uint8_t data[8] = {0};
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &len);
        ESP_LOGI(TAG, "report rate dsc write (handle=0x%04x, %uB: %02x %02x)",
                 attr_handle, (unsigned)len, data[0], len > 1 ? data[1] : 0);
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* 写缓冲按 ATT 载荷上限取（MTU 512 → 有效载荷 509B）：升级数据块与复合
     * 帧都可能超过 128B，短于整块的缓冲会让上层把整块记成 0 字节。NimBLE
     * 主机任务串行处理各连接的 ATT 写，静态缓冲不跨任务共享。 */
    static uint8_t wbuf[512];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, wbuf, sizeof(wbuf), &len);
    memset(&wbuf[len], 0, sizeof(wbuf) - len);
    /* 主机输出的原始字节先过采集通道：这是布局解析与结构化事件之前的最
     * 原始数据（串口 `capture on` 打开，关闭时只有一次布尔读）。 */
    dp_capture_host_write(capture_channel(tag), wbuf, len);
    switch (tag) {
    case CHR_RUMBLE:
        ns2_session_on_output(wbuf, len, conn_handle);
        break;
    case CHR_CMD:
        ns2_session_on_command(wbuf, len, NS2_FRAME_TRANSPORT_BLE, conn_handle);
        break;
    case CHR_COMPOSITE:
        ns2_session_on_composite(wbuf, len, conn_handle);
        break;
    case CHR_FWUPG: {
        /* 固件升级数据块（0x0018 WRITE NO RSP）：整块交给假升级会话逐块留痕。 */
        const size_t total = OS_MBUF_PKTLEN(ctxt->om);
        if (total > sizeof(wbuf)) {
            ESP_LOGW(TAG, "fwupd block %uB over %uB buffer", (unsigned)total,
                     (unsigned)sizeof(wbuf));
        }
        ns2_session_on_fw_upgrade(wbuf, len, conn_handle);
        break;
    }
    case CHR_BASE_CONFIG:
        log_write_head("vendor base config write", tag, ctxt->om);
        break;
    case CHR_EXT22:
    case CHR_EXT26:
    case CHR_EXT2A:
    case CHR_EXT2C:
    case CHR_EXT2E:
    case CHR_EXT32:
        /* 未知功能特征值（0x0022-0x0032 段）：接受写入即认可。 */
        log_write_head("ext chr write", tag, ctxt->om);
        break;
    default:
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    return 0;
}

/** 注册回调：捕获关键特征值句柄并打印整表，供与 GATT 属性表比对。 */
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

/** 接收连接的实例是否需要继续广播：Pro 单身份（含同址的另一实例）一并停止。 */
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
        struct ble_gap_conn_desc desc = {0};
        uint8_t identity = NS2_ID_PRO;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            const int inst = adv_instance_by_addr(desc.our_ota_addr.val);
            if (inst >= 0) {
                identity = s_adv_identity[inst];
                stop_advertising_for(identity);
            } else {
                /* 地址对不上任何实例（如实例已停）：整条链路都不必再广播。 */
                ble_controller_adv_stop();
            }
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->conn_handle = event->connect.conn_handle;
        slot->identity = identity;
        slot->conn_itvl = desc.conn_itvl;
        slot->mtu = ble_att_mtu(event->connect.conn_handle);
        ESP_LOGI(TAG, "ACL connected (conn=%u, identity=%s, itvl=%u units / %u us, mtu=%u, "
                 "peer id/ota type %u/%u)",
                 event->connect.conn_handle, ns2_identity_name(identity),
                 desc.conn_itvl, (unsigned)desc.conn_itvl * 1250u, (unsigned)slot->mtu,
                 desc.peer_id_addr.type, desc.peer_ota_addr.type);
        ns2_session_on_connect(event->connect.conn_handle, identity);
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
        /* 连接参数由主机发起：主机常把连接压到 4 单位（5ms，亚规范间隔），
         * 控制器侧需放行（CONFIG_BT_CTRL_BLE_MIN_CONN_INTERVAL_ENABLE）；
         * 这里只观测，不反向请求（NimBLE 主机侧拒绝 itvl < 6 的请求）。
         * 输入被主机采用的门槛是 0x0C/0x04 特性启用，不是间隔（ADR 0023）。 */
        conn_slot_t *slot = conn_slot(event->conn_update.conn_handle);
        struct ble_gap_conn_desc desc = {0};
        uint16_t itvl = 0;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            itvl = desc.conn_itvl;
        }
        if (slot != NULL) {
            slot->conn_itvl = itvl;
            slot->mtu = ble_att_mtu(event->conn_update.conn_handle);
        }
        ESP_LOGI(TAG, "conn update (conn=%u, itvl=%u units / %u us, mtu=%u, status=%d)",
                 event->conn_update.conn_handle, itvl, (unsigned)itvl * 1250u,
                 (unsigned)ble_att_mtu(event->conn_update.conn_handle),
                 event->conn_update.status);
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        const conn_slot_t *slot = conn_slot(event->enc_change.conn_handle);
        ESP_LOGI(TAG, "encryption change (conn=%u, identity=%s, status=%d)",
                 event->enc_change.conn_handle,
                 ns2_identity_name(slot ? slot->identity : NS2_ID_PRO),
                 event->enc_change.status);
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        conn_slot_t *slot = conn_slot(event->disconnect.conn.conn_handle);
        const uint8_t identity = slot ? slot->identity : NS2_ID_PRO;
        if (slot != NULL) {
            memset(slot, 0, sizeof(*slot));
        }
        ESP_LOGI(TAG, "disconnected reason=0x%02x (identity=%s)",
                 event->disconnect.reason, ns2_identity_name(identity));
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
                /* 专用输入通道：记下主机订的句柄，通知就往它发。 */
                slot->input_priv_notify = event->subscribe.cur_notify != 0;
                slot->input_priv_handle =
                    slot->input_priv_notify ? event->subscribe.attr_handle : 0;
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
        /* 忽略重复的 SMP 配对请求：标准 SMP 只用于链路加密，配对密钥由 Command 0x15 承担。 */
        ESP_LOGW(TAG, "repeat pairing ignored");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}

/** 启动一个广播实例。addr 非 NULL 时以该静态随机地址广播（advaddr 对账）。 */
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
    /* legacy PDU 才是主机看得见、也认得住的形式（ADV_IND 可连接可扫描、
     * 附空 SCAN_RSP，广播间隔 30 ms）；扩展 PDU 按规范不可同时置可连接与
     * 可扫描，主机侧完全看不见。 */
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

void ble_controller_adv_start(uint8_t instance, uint8_t identity,
                              const uint8_t payload[31], const uint8_t addr[6])
{
    if (instance >= ADV_INSTANCE_MAX) {
        return;
    }
    /* 实例身份由会话层显式给出（只有 Pro，双实例同址）。addr 为 NULL 时
     * 沿用公共伪装地址。 */
    s_adv_identity[instance] = identity;
    ESP_LOG_BUFFER_HEX(TAG, payload, 31);
    /* 默认形态是 legacy PDU（发现广播是可连接 + 可扫描的 ADV_IND）：扩展 PDU
     * 的实例在主机侧完全看不见——同一份载荷、同一个 public 地址，切成 legacy
     * PDU 后主机立刻连接并跑完 0x15 配对。对账开关可强制扩展形态做反向验证。 */
    const int legacy_pdu = s_adv_pdu_form == BLE_CTL_ADV_PDU_EXTENDED ? 0 : 1;
    adv_start_instance(instance, legacy_pdu, payload, addr);
}

bool ble_controller_adv_running(uint8_t identity)
{
    for (size_t i = 0; i < ADV_INSTANCE_MAX; i++) {
        if (s_adv_identity[i] == identity && ble_gap_ext_adv_active((uint8_t)i)) {
            return true;
        }
    }
    return false;
}

void ble_controller_adv_stop(void)
{
    for (uint8_t i = 0; i < ADV_INSTANCE_MAX; i++) {
        ble_gap_ext_adv_stop(i);
    }
}

void ble_controller_adv_stop_identity(uint8_t identity)
{
    stop_advertising_for(identity);
}

static void notify(uint16_t conn_handle, uint16_t attr_handle, bool enabled,
                   const uint8_t *data, size_t len)
{
    if (!enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        conn_slot_t *slot = conn_slot(conn_handle);
        if (slot != NULL) {
            slot->tx_fail++;
            slot->tx_rc = -1;
        }
        return;
    }
    /* 返回码必须记录：MTU 不足时 ble_gatts_notify_custom 直接失败，静默吞掉
     * 会让「主机已订阅但收不到输入」无法定位。 */
    const int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if (rc != 0) {
        conn_slot_t *slot = conn_slot(conn_handle);
        if (slot != NULL) {
            if (slot->tx_fail == 0) {
                ESP_LOGW(TAG, "notify failed rc=%d (conn=%u, handle=0x%04x, len=%u, mtu=%u)",
                         rc, conn_handle, attr_handle, (unsigned)len,
                         (unsigned)ble_att_mtu(conn_handle));
            }
            slot->tx_fail++;
            slot->tx_rc = rc;
        }
    }
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

/** 只刷新输入快照、不发通知：主机用 READ 轮询输入通道时读到的是这份值，
 *  特性未启用的链路上也必须跟着数据面更新（否则读到的是全零）。 */
void ble_controller_store_input(uint16_t conn_handle, uint8_t report_format,
                                const uint8_t report[63])
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return;
    }
    uint8_t *dst = report_format == 5 ? slot->last_input05 : slot->last_input_priv;
    memcpy(dst, report, 63);
}

/** 专用输入通道通知（0x09 报文体）：往主机本次订阅的那个句柄发；未订阅时
 *  退回本通道句柄，只刷新 READ 快照。 */
void ble_controller_notify_input_09(uint16_t conn_handle, const uint8_t report[63])
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return;
    }
    memcpy(slot->last_input_priv, report, 63);
    const uint16_t handle = slot->input_priv_handle != 0
                                ? slot->input_priv_handle
                                : s_h.input09;
    notify(conn_handle, handle, slot->input_priv_notify, report, 63);
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
    return report_format == 5 ? slot->input05_notify : slot->input_priv_notify;
}

bool ble_controller_conn_itvl(uint16_t conn_handle, uint16_t *out_itvl)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL || out_itvl == NULL) {
        return false;
    }
    *out_itvl = slot->conn_itvl;
    return true;
}

bool ble_controller_conn_stats(uint16_t conn_handle, uint16_t *out_itvl, uint16_t *out_mtu,
                               uint32_t *out_tx_fail, int *out_tx_rc, bool *out_encrypted)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL) {
        return false;
    }
    if (out_itvl != NULL) {
        *out_itvl = slot->conn_itvl;
    }
    if (out_mtu != NULL) {
        *out_mtu = slot->mtu;
    }
    if (out_tx_fail != NULL) {
        *out_tx_fail = slot->tx_fail;
    }
    if (out_tx_rc != NULL) {
        *out_tx_rc = slot->tx_rc;
    }
    if (out_encrypted != NULL) {
        struct ble_gap_conn_desc desc = {0};
        *out_encrypted = ble_gap_conn_find(conn_handle, &desc) == 0 && desc.sec_state.encrypted;
    }
    return true;
}

bool ble_controller_last_input(uint16_t conn_handle, uint8_t report_format, uint8_t *out)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL || out == NULL) {
        return false;
    }
    memcpy(out, report_format == 5 ? slot->last_input05 : slot->last_input_priv, 63);
    return true;
}

/** 主机本次订阅的专用输入通道句柄（0 = 未订阅），供串口 link 显示。 */
bool ble_controller_input_priv_handle(uint16_t conn_handle, uint16_t *out_handle)
{
    conn_slot_t *slot = conn_slot(conn_handle);
    if (slot == NULL || out_handle == NULL) {
        return false;
    }
    *out_handle = slot->input_priv_handle;
    return true;
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

    /* NimBLE 对每条 ATT 通知都打一行 INFO（连接期间约 200 行/秒）：会把串口
     * 日志淹掉、排查时看不到自己的事件，也会给上报循环增加格式化开销。
     * 只留 WARN 及以上。 */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

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

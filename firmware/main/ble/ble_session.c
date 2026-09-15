#include "ble_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_store.h"
#include "psa/crypto.h"

#include "app_config.h"
#include "ble_controller.h"
#include "ble_creds.h"
#include "dp_plane.h"
#include "ns2_adv.h"
#include "ns2_identity.h"
#include "ns2_frames.h"
#include "ns2_output.h"
#include "ns2_report.h"
#include "ns2_serial.h"
#include "pad_state.h"

static const char *TAG = "remapad_blses";

#define ANSWER_PREFIX_LEN 14
#define FACTORY_SIZE 2048u

/** 会话状态：休眠/唤醒广播顺延到后续里程碑（见 docs/ROADMAP.md M3）。 */
typedef enum {
    SESSION_ADV_DISCOVERY = 0,
    SESSION_ADV_RECONNECT,
    SESSION_CONNECTED_WAIT_PAIR,
    SESSION_NORMAL,
} session_state_t;

/** 单条连接的会话状态（Pro 1 槽 / JoyCon 组合 2 槽）。 */
#define SESSION_MAX 2

typedef struct {
    bool active;
    uint16_t conn_handle;
    uint8_t identity; /* ns2_identity_t */
    session_state_t state;
    uint8_t report_format;
    uint8_t feature_mask;
    uint8_t player_leds;
    int64_t last_activity_us;
    /** 0x15 配对会话中间态：主机 MAC 与派生 LTK（线格式字节序）。 */
    uint8_t pair_host_mac[6];
    bool pair_mac_ready;
    uint8_t pair_ltk[16];
    bool pair_ltk_ready;
    /** 已投递的输入报告数（主机订阅后计数），供控制面诊断。 */
    uint32_t reports;
    /** 主机已发 0x0c/0x04 启用特性：输入报文被采用的门槛，未启用不发。 */
    bool features_enabled;
    /** 休眠看门狗计数：每秒 +1，主机启用特性即清零。 */
    uint8_t dormant_ticks;
} session_slot_t;

static struct {
    session_slot_t sess[SESSION_MAX];
    uint8_t own_mac[6];
    bool synced;
    /** 设备形态：NS2_ID_PRO（单连接）或 NS2_ID_JOYCON_L（JoyCon 组合，
     * 左右双连接；此字段存「形态」而非单只身份）。 */
    uint8_t device_mode;
    uint32_t body_color;
    uint32_t button_color;
    uint32_t grip_color;
    bool pairing_mode;
    /** 当前形态的身份是否都已连上（成对在线行只打印一次）。 */
    bool pair_online_logged;
} s_ses;

/** 当前手柄身份集合：Pro = {PRO}；JoyCon 组合 = {JOYCON_L, JOYCON_R}。 */
static size_t mode_identities(ns2_identity_t *out)
{
    if (s_ses.device_mode == NS2_ID_PRO) {
        out[0] = NS2_ID_PRO;
        return 1;
    }
    out[0] = NS2_ID_JOYCON_L;
    out[1] = NS2_ID_JOYCON_R;
    return 2;
}

static bool identity_in_mode(ns2_identity_t identity)
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == identity) {
            return true;
        }
    }
    return false;
}

/** 身份的 PID（controller.md 手柄型号表）：Pro 0x2069；Joy-Con 2 (L) 0x2067、
 * (R) 0x2066。 */
static uint16_t identity_pid(ns2_identity_t identity)
{
    switch (identity) {
    case NS2_ID_JOYCON_L:
        return 0x2067;
    case NS2_ID_JOYCON_R:
        return 0x2066;
    default:
        return 0x2069;
    }
}

/** 身份的序列号（3 字母前缀 + 10 位数字，末位校验位由 ns2_serial_build 补齐，
 * 命名规则见 controller.md §7.2）。不同手柄/配色批次预期不同序列号；
 * 当前每种身份一组固定值，颜色选择实装后再随配置派生。 */
static const char *identity_serial_prefix(ns2_identity_t identity)
{
    switch (identity) {
    case NS2_ID_JOYCON_L:
        return "HBW";
    case NS2_ID_JOYCON_R:
        return "HCW";
    default:
        return "HEJ";
    }
}

static const char *identity_serial_digits(ns2_identity_t identity)
{
    switch (identity) {
    case NS2_ID_JOYCON_L:
        return "1006701234";
    case NS2_ID_JOYCON_R:
        return "1006801234";
    default:
        return "7100112345";
    }
}

static uint8_t s_factory[FACTORY_SIZE];

/** 0x13000 出厂数据块（实机抓包布局）：`01 00` + 序列号@2 + `00 00`
 * + VID/PID@18 + 版本@22 + 机身配色@25，尾部 0xFF。按身份各留一份，
 * 指令处理按连接身份取用（JoyCon 双连接各自上报自己的出厂信息）。 */
#define MEM_FACTORY_MAX NS2_ID_COUNT
static uint8_t s_mem_factory[MEM_FACTORY_MAX][64];

/** 0x7E40 首块（实机抓包：主机初始化经 0x02/0x04 读取）：6B 头 + 14B
 * 序列号@6 + 2B 保留@20 + 4B VID/PID@22 + 3B 版本@26 + 12B 配色@29。 */
static uint8_t s_mem_7e40[MEM_FACTORY_MAX][64];

/** 0x13080 / 0x130C0 摇杆校准块（实机抓包字节，64B）。 */
static const uint8_t s_mem_cal80[64] = {
    0x01, 0xad, 0xd9, 0x9a, 0x55, 0x56, 0x65, 0xa0, 0x00, 0x0a, 0xa0, 0x00,
    0x0a, 0xe2, 0x20, 0x0e, 0xe2, 0x20, 0x0e, 0x9a, 0xad, 0xd9, 0x9a, 0xad,
    0xd9, 0x0a, 0xa5, 0x50, 0x0a, 0xa5, 0x50, 0x2f, 0xf6, 0x62, 0x2f, 0xf6,
    0x62, 0x0a, 0xff, 0xff, 0xb3, 0x67, 0x83, 0x2e, 0x66, 0x5e, 0x3a, 0x06,
    0x5f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
};
static const uint8_t s_mem_calc0[64] = {
    0x01, 0xad, 0xd9, 0x9a, 0x55, 0x56, 0x65, 0xa0, 0x00, 0x0a, 0xa0, 0x00,
    0x0a, 0xe2, 0x20, 0x0e, 0xe2, 0x20, 0x0e, 0x9a, 0xad, 0xd9, 0x9a, 0xad,
    0xd9, 0x0a, 0xa5, 0x50, 0x0a, 0xa5, 0x50, 0x2f, 0xf6, 0x62, 0x2f, 0xf6,
    0x62, 0x0a, 0xff, 0xff, 0x2c, 0x08, 0x84, 0xd1, 0x65, 0x63, 0x2a, 0x26,
    0x62, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
};

/** 帧内字节序列整体反转（MAC、AES 挑战与注入 LTK 的字节序变换，controller.md §3.2）。 */
static void reverse_bytes(const uint8_t *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = in[n - 1 - i];
    }
}

/** AES-128-ECB 单块加密（PSA Crypto；IDF 6.1 的 mbedtls 4 已移除 legacy API）。 */
static bool aes_ecb_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    static bool crypto_inited;
    if (!crypto_inited) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            return false;
        }
        crypto_inited = true;
    }
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_key_id_t key_id = 0;
    if (psa_import_key(&attrs, key, 16, &key_id) != PSA_SUCCESS) {
        return false;
    }
    size_t olen = 0;
    const psa_status_t status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING,
                                                   in, 16, out, 16, &olen);
    psa_destroy_key(key_id);
    return status == PSA_SUCCESS && olen == 16;
}

/** 出厂数据区（序列号、VID/PID、机身配色与摇杆校准，controller.md §7.2）。
 * 校准取中位 2048、行程 ±2047/2048，与编码器 0-4095 直发语义保持 1:1。
 * 序列号、版本与配色按当前模式的每个身份各生成一份。 */
static void factory_init(void)
{
    memset(s_factory, 0xFF, sizeof(s_factory));
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    const uint32_t body = s_ses.body_color != 0 ? s_ses.body_color : 0x232323;
    const uint32_t button = s_ses.button_color != 0 ? s_ses.button_color : 0x3c3c3c;
    const uint32_t grip = s_ses.grip_color != 0 ? s_ses.grip_color : 0x2e2e2e;
    static const uint8_t stick_cal[9] = {0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80};
    memcpy(&s_factory[0x00A8], stick_cal, sizeof(stick_cal));
    memcpy(&s_factory[0x00E8], stick_cal, sizeof(stick_cal));

    const uint8_t *ver = app_config_get()->fw_version;
    const uint8_t colors[12] = {
        (uint8_t)(body >> 16), (uint8_t)(body >> 8), (uint8_t)(body),
        (uint8_t)(button >> 16), (uint8_t)(button >> 8), (uint8_t)(button),
        (uint8_t)(button >> 16), (uint8_t)(button >> 8), (uint8_t)(button),
        (uint8_t)(grip >> 16), (uint8_t)(grip >> 8), (uint8_t)(grip),
    };
    for (size_t i = 0; i < n; i++) {
        const ns2_identity_t identity = ids[i];
        const uint16_t pid = identity_pid(identity);
        char serial[15];
        ns2_serial_build(identity_serial_prefix(identity),
                         identity_serial_digits(identity), serial);

        /* 0x7E40 首块：布局按实机抓包注释；6B 头内容未验证，取 `01 00`
         * 前缀填零。此前未提供该块，主机初始化读取失败可能正是固件更新
         * 提示的诱因（版本读不到即视为旧固件）。 */
        uint8_t *blk = s_mem_7e40[identity];
        memset(blk, 0xFF, 64);
        blk[0] = 0x01;
        blk[1] = 0x00;
        memcpy(&blk[6], serial, 14);
        blk[22] = 0x7E;
        blk[23] = 0x05;
        blk[24] = (uint8_t)(pid & 0xFF);
        blk[25] = (uint8_t)(pid >> 8);
        blk[26] = ver[0];
        blk[27] = ver[1];
        blk[28] = ver[2];
        memcpy(&blk[29], colors, sizeof(colors));

        uint8_t *mem = s_mem_factory[identity];
        memset(mem, 0xFF, 64);
        mem[0] = 0x01;
        mem[1] = 0x00;
        memcpy(&mem[2], serial, 14);
        mem[18] = 0x7E;
        mem[19] = 0x05;
        mem[20] = (uint8_t)(pid & 0xFF);
        mem[21] = (uint8_t)(pid >> 8);
        mem[22] = ver[0];
        mem[23] = ver[1];
        mem[24] = ver[2];
        memcpy(&mem[25], colors, sizeof(colors));
        ESP_LOGI(TAG, "factory[%u] serial=%s pid=0x%04x fw=%u.%u.%u",
                 (unsigned)identity, serial, pid, ver[0], ver[1], ver[2]);
    }
}

/** 唤醒窗口时长：真机按键唤醒只发约 2 秒的 0x81 广播，这里取 10 秒——主机
 *  的唤醒扫描窗口远长于 2 秒，窗口太短会错过；连接建立即提前结束窗口，
 *  所以主机在线时不会多打扰它。 */
#define NS2_WAKE_BURST_US (10 * 1000000LL)

/** 休眠看门狗：已订阅输入但主机始终没发 0x0c/0x04（启用特性）持续这么多
 *  秒（tick 每 1 秒一次），判定为主机不采用输入的休眠连接——握把页的快捷
 *  回连正是这个形态（实测 itvl=4 但未启用的链路按键同样无效）。正常握手
 *  在订阅前后一两秒内就会启用特性，15 秒足够宽。 */
#define NS2_DORMANT_TICKS 15

/** 每次上电允许的休眠断开次数上限：超过即放弃（避免与主机反复互相拉扯）。 */
#define NS2_DORMANT_MAX_DROPS 3

/** 唤醒窗口（0 = 未开）：窗口内广播形态为 0x81 的唤醒广播。 */
static ns2_adv_wake_window_t s_wake_win;

/** 本次上电已执行的休眠断开次数（上限 NS2_DORMANT_MAX_DROPS）。 */
static uint8_t s_dormant_drops;

static bool wake_burst_active(void)
{
    return ns2_adv_wake_window_active(&s_wake_win, esp_timer_get_time());
}

static bool mac_all_zero(const uint8_t mac[6])
{
    for (size_t i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

/** 身份的广播形态：手动配对模式与未配对身份发发现广播；已配对身份平时发
 * 回连广播（状态位 0x00），只有显式唤醒突发期间才发 0x81 的唤醒广播。
 * 地址优先取最近一次连接记录到的主机地址，其次取最近一条「非全零」凭证：
 * NVS 里存在计数虚高、尾部记录全零的历史表，直接取最近一条会写出全零地址
 * 的广播——主机既不会回连也不会被唤醒。 */
static ns2_adv_mode_t adv_mode_for(ns2_identity_t identity, const uint8_t **out_mac)
{
    static uint8_t s_adv_host_mac[6];
    if (s_ses.pairing_mode) {
        *out_mac = NULL;
        return NS2_ADV_DISCOVERY;
    }
    /* 凭证优先：记录值可能被普通 BLE 主机（PC/手机）污染，凭证只会在 NS2
     * 配对交换里写入。 */
    const size_t count = ble_creds_count(identity);
    for (size_t i = count; i > 0; i--) {
        const ns2_cred_record_t *rec = ble_creds_get(identity, i - 1);
        if (rec != NULL && !mac_all_zero(rec->mac)) {
            memcpy(s_adv_host_mac, rec->mac, sizeof(s_adv_host_mac));
            *out_mac = s_adv_host_mac;
            return ns2_adv_choose_mode(true, wake_burst_active());
        }
    }
    if (ble_creds_host_mac(identity, s_adv_host_mac)) {
        *out_mac = s_adv_host_mac;
        return ns2_adv_choose_mode(true, wake_burst_active());
    }
    *out_mac = NULL;
    return ns2_adv_choose_mode(false, wake_burst_active());
}

static const char *adv_mode_name(ns2_adv_mode_t mode)
{
    switch (mode) {
    case NS2_ADV_WAKE:
        return "wake";
    case NS2_ADV_RECONNECT:
        return "reconnect";
    default:
        return "discovery";
    }
}

/** 启动一个身份的广播：Pro 单身份占两个实例（扩展 + legacy PDU）共用公共
 * 伪装地址，JoyCon 组合左右各占一个实例（静态随机地址）。 */
static void adv_start_identity(size_t index, ns2_identity_t identity,
                               const uint8_t payload[NS2_ADV_PAYLOAD_LEN])
{
    ns2_identity_t ids[2];
    if (mode_identities(ids) == 1) {
        ble_controller_adv_start(0, (uint8_t)NS2_ID_PRO, payload, NULL);
        ble_controller_adv_start(1, (uint8_t)NS2_ID_PRO, payload, NULL);
        return;
    }
    uint8_t addr[6];
    ns2_identity_adv_addr(s_ses.own_mac, identity, addr);
    ble_controller_adv_start((uint8_t)index, identity, payload, addr);
}

/** 按凭证状态恢复广播：Pro 双实例（扩展 + legacy PDU）共用公共伪装地址；
 * JoyCon 双身份各占一个实例（静态随机地址，legacy PDU）。已配对身份发
 * 回连广播等待主机回连，未配对身份发发现广播待主机搜索配对。 */
static void resume_advertising(void)
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        uint8_t adv[NS2_ADV_PAYLOAD_LEN];
        const uint8_t *mac = NULL;
        const ns2_adv_mode_t mode = adv_mode_for(ids[i], &mac);
        ns2_adv_payload(adv, identity_pid(ids[i]), mode, mac);
        adv_start_identity(i, ids[i], adv);
        ESP_LOGI(TAG, "resume: identity %u %s advertising (%u creds)",
                 (unsigned)ids[i], adv_mode_name(mode), (unsigned)ble_creds_count(ids[i]));
    }
}

void ns2_session_wake_request(void)
{
    if (s_ses.pairing_mode) {
        ESP_LOGI(TAG, "wake burst ignored (pairing mode)");
        return;
    }
    ns2_adv_wake_window_open(&s_wake_win, esp_timer_get_time(), NS2_WAKE_BURST_US);
    ESP_LOGI(TAG, "wake burst started (%u ms)", (unsigned)(NS2_WAKE_BURST_US / 1000));
    if (ble_controller_connected()) {
        /* 已连接时唤醒请求按「重新连接」处理：主机从「更改握法/顺序」页面
         * 连上来的会话不会采用输入报文（controller.md §12），断一次让主机
         * 按回连路径重新连上来；断开事件随即重启广播，此时窗口内发 0x81。 */
        ESP_LOGI(TAG, "wake: dropping current link to force a reconnect");
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
        return;
    }
    resume_advertising();
}

void ns2_session_on_sync(const uint8_t own_mac[6])
{
    factory_init();
    s_ses.synced = true;
    memcpy(s_ses.own_mac, own_mac, 6);
    ESP_LOGI(TAG, "host synced, own MAC %02x:%02x:%02x:%02x:%02x:%02x",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
    /* 已配对的上电：先开一段唤醒窗口，让休眠中的主机被叫醒并按回连路径
     * 连上来（从配对页面连上来的会话输入会被主机侧阻塞，见 controller.md
     * §12）；窗口内连上即结束窗口，超时后退回 0x00 的回连形态等主机回连。 */
    if (ns2_session_paired()) {
        ns2_adv_wake_window_open(&s_wake_win, esp_timer_get_time(), NS2_WAKE_BURST_US);
        ESP_LOGI(TAG, "boot wake window opened (%u ms)",
                 (unsigned)(NS2_WAKE_BURST_US / 1000));
    }
    resume_advertising();
}

/** 连接空闲超时（微秒）：主机连上后会立刻跑初始化序列（毫秒级到达），
 * 手机/PC 的自动回连则连上后无任何协议活动；超时未活动即断开，
 * 兼顾主机初始化与回连骚扰清理。主机连接地址是随机地址，
 * 无法按 OUI 识别，不能再用地址白名单。 */
#define HOST_IDLE_TIMEOUT_US (3 * 1000000LL)

static session_slot_t *session_by_conn(uint16_t conn_handle)
{
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active && s_ses.sess[i].conn_handle == conn_handle) {
            return &s_ses.sess[i];
        }
    }
    return NULL;
}

static session_slot_t *session_by_identity(uint8_t identity)
{
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active && s_ses.sess[i].identity == identity) {
            return &s_ses.sess[i];
        }
    }
    return NULL;
}

/** 会话进入 normal（凭证匹配回连或本会话完成握手）：打印可观测的状态行。 */
static void log_session_normal(const session_slot_t *ses)
{
    ESP_LOGI(TAG, "%s session normal (conn=%u, fmt=0x%02x)",
             ns2_identity_name(ses->identity), ses->conn_handle, ses->report_format);
}

/** 当前形态的所有身份都已连上时打印一次成对在线行：主机 Grip 页组合前
 *  的证据；任一身份断开后重新武装。 */
static void log_pair_online(void)
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        if (session_by_identity(ids[i]) == NULL) {
            return;
        }
    }
    if (s_ses.pair_online_logged) {
        return;
    }
    s_ses.pair_online_logged = true;
    char list[24];
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const int written = snprintf(&list[used], sizeof(list) - used,
                                     i == 0 ? "%s" : " + %s",
                                     ns2_identity_name(ids[i]));
        if (written <= 0 || (size_t)written >= sizeof(list) - used) {
            break;
        }
        used += (size_t)written;
    }
    ESP_LOGI(TAG, "all identities online: %s", list);
}

/** 身份对外广播地址（NimBLE 存储序）；host 未同步时地址尚未确定。 */
static bool identity_mac(uint8_t identity, uint8_t out[6])
{
    if (!s_ses.synced) {
        return false;
    }
    ns2_identity_adv_addr(s_ses.own_mac, identity, out);
    return true;
}

void ns2_session_touch(uint16_t conn_handle)
{
    session_slot_t *slot = session_by_conn(conn_handle);
    if (slot != NULL) {
        slot->last_activity_us = esp_timer_get_time();
    }
}

bool ns2_session_conn_idle_expired(uint16_t conn_handle)
{
    const session_slot_t *slot = session_by_conn(conn_handle);
    return slot != NULL && slot->state == SESSION_CONNECTED_WAIT_PAIR &&
           esp_timer_get_time() - slot->last_activity_us > HOST_IDLE_TIMEOUT_US;
}

/** 将派生 LTK 注入 NimBLE bonding store（随机数与 EDIV 全 0，BLE 链路
 * 加密用的 LTK 为线序 A1 XOR B1）；主机配对完成或回连后调用。 */
/** LTK 注入形态：0 = 反转后写入（默认，与参考实现一致），1 = 原样写入。
 *  主机连上但 `link` 显示 enc=0 时用它做现场 A/B。 */
static uint8_t s_ltk_form;

static void inject_ltk_to_ble_store(const uint8_t host_mac[6], const uint8_t ltk[16])
{
    struct ble_store_value_sec sec;
    memset(&sec, 0, sizeof(sec));
    sec.bond_count = 1;
    sec.key_size = 16;
    sec.ltk_present = 1;
    if (s_ltk_form == 0) {
        /* 参考实现形态：会话里存的是 AES 密钥形态（A1^B1 反序），写栈前再反
         * 转回主机存储形态（zhantss/ESP32-BLE5-NSController-Emulator）。 */
        reverse_bytes(ltk, sec.ltk, 16);
    } else {
        /* 研究仓库的 .ltk（用于解密真机链路）恰是反序形态：按原样写入。 */
        memcpy(sec.ltk, ltk, 16);
    }
    memcpy(sec.peer_addr.val, host_mac, 6);
    sec.rand_num = 0;
    sec.ediv = 0;
    sec.authenticated = 1;
    sec.sc = 1;
    /* 主机的地址类型（公有/随机）也要对上：栈按「地址 + 类型」查 LTK，类型
     * 不一致会查不到密钥、加密请求被否掉（表现为主机连上但不认这台手柄）。
     * 主机可能用两个地址中的任意一个，两种类型都登记一份。 */
    static const uint8_t types[2] = {BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM};
    for (size_t i = 0; i < 2; i++) {
        sec.peer_addr.type = types[i];
        ble_store_write_our_sec(&sec);
        ble_store_write_peer_sec(&sec);
    }
}

void ns2_session_set_ltk_form(uint8_t form)
{
    s_ltk_form = form == 0 ? 0 : 1;
}

uint8_t ns2_session_ltk_form(void)
{
    return s_ltk_form;
}

void ns2_session_on_connect(uint16_t conn_handle, uint8_t identity)
{
    session_slot_t *slot = NULL;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (!s_ses.sess[i].active) {
            slot = &s_ses.sess[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "no session slot for conn %u", conn_handle);
        ble_controller_disconnect(BLE_CTL_DISCONNECT_CONN_FAIL);
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->conn_handle = conn_handle;
    slot->identity = identity;
    slot->report_format = NS2_REPORT_ID_09;
    /* 主机已回连：唤醒窗口立即结束，后继广播回到 0x00 的回连形态。 */
    if (wake_burst_active()) {
        ns2_adv_wake_window_close(&s_wake_win);
        ESP_LOGI(TAG, "wake burst finished (host connected)");
    }

    uint8_t peer[6] = {0};
    const bool have_peer = ble_controller_peer_mac(conn_handle, peer);
    const ns2_cred_record_t *matched = NULL;
    if (have_peer) {
        for (size_t i = 0; i < ble_creds_count(identity); i++) {
            if (memcmp(ble_creds_get(identity, i)->mac, peer, 6) == 0) {
                matched = ble_creds_get(identity, i);
                break;
            }
        }
    }
    if (matched != NULL) {
        /* 命中配对凭证：确认是 NS2 主机，记下它当前使用的地址（回连/唤醒
         * 广播必须带主机自己的地址）。普通 BLE 主机（PC/手机）不写这条，
         * 否则它们会把回连目标改成自己。 */
        ble_creds_note_host_mac(identity, peer);
    }
    ESP_LOGI(TAG, "connected (conn=%u, identity=%s, peer %02x:%02x:%02x:%02x:%02x:%02x, %s)",
             conn_handle, ns2_identity_name(identity),
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
             matched ? "paired host" : "unpaired host");
    slot->state = matched ? SESSION_NORMAL : SESSION_CONNECTED_WAIT_PAIR;
    slot->last_activity_us = esp_timer_get_time();
    if (matched) {
        /* 回连：把 NVS 里的 LTK 重新注入本周期 NimBLE RAM store。 */
        inject_ltk_to_ble_store(matched->mac, matched->ltk);
        log_session_normal(slot);
    }
    ESP_LOGI(TAG, "waiting host init sequence");
    log_pair_online();
}

void ns2_session_on_connect_fail(void)
{
    if (!ble_controller_connected()) {
        resume_advertising();
    }
}

void ns2_session_on_disconnect(uint16_t conn_handle, uint8_t identity)
{
    session_slot_t *slot = session_by_conn(conn_handle);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    /* 成对在线状态被打破，下一次全部在线时再打印一次。 */
    s_ses.pair_online_logged = false;
    if (!ble_controller_connected()) {
        resume_advertising();
    } else if (identity != NS2_ID_PRO && identity_in_mode(identity)) {
        /* JoyCon 组合：另一只仍在线，只恢复断开身份的广播等它回连。 */
        uint8_t adv[NS2_ADV_PAYLOAD_LEN];
        const uint8_t *mac = NULL;
        const ns2_adv_mode_t mode = adv_mode_for(identity, &mac);
        ns2_adv_payload(adv, identity_pid(identity), mode, mac);
        adv_start_identity(identity == NS2_ID_JOYCON_L ? 0 : 1, identity, adv);
    }
}

/** SPI 模拟内存映射块。0x13040 与 0x13100 为实机固定内容；0x13060 与
 * 用户自定义校准区（0x1FC000 运动 / 0x1FC040 主摇杆 / 0x1FC060 副摇杆）
 * 未经写入即未初始化，长度 0、读出为全 0xFF。出厂数据块按当前连接的
 * 身份提供（JoyCon 双连接各自读到自己的序列号 / PID / 配色）。 */
typedef struct {
    uint32_t start;
    size_t len;
    const uint8_t *data;
} mem_map_entry_t;
#define MEM_MAP_LEN 9

static void build_mem_map(mem_map_entry_t *map, uint8_t identity)
{
    map[0].start = 0x007E40u;
    map[0].len = 64;
    map[0].data = s_mem_7e40[identity];
    map[1].start = 0x013000u;
    map[1].len = 64;
    map[1].data = s_mem_factory[identity];
    map[2].start = 0x013040u;
    map[2].len = 16;
    map[2].data = (const uint8_t[]){0x3B, 0xE0, 0xD3, 0x41, 0xC6, 0x60, 0x6A, 0xBC,
                                    0x4D, 0xD7, 0xA2, 0xBB, 0x71, 0x1E, 0xDD, 0x37};
    map[3].start = 0x013060u;
    map[3].len = 0;
    map[3].data = NULL;
    map[4].start = 0x013080u;
    map[4].len = sizeof(s_mem_cal80);
    map[4].data = s_mem_cal80;
    map[5].start = 0x0130c0u;
    map[5].len = sizeof(s_mem_calc0);
    map[5].data = s_mem_calc0;
    map[6].start = 0x013100u;
    map[6].len = 24;
    map[6].data = (const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0xA6, 0xF2, 0x62, 0xBD,
                                    0xA8, 0x00, 0x08, 0x3D, 0x2F, 0xED, 0x20, 0x41};
    map[7].start = 0x01fc000u;
    map[7].len = 0;
    map[7].data = NULL;
    map[8].start = 0x01fc060u;
    map[8].len = 0;
    map[8].data = NULL;
}

/** Command 0x02 SPI 读取：应答体 = 回显请求 [8:16] 的 8B magic（[1] 清零）
 * + 命中数据。地址为请求 [12:15] 的 3 字节小端，按映射区间重叠取数，
 * 命中范围内未覆盖的部分补 0xFF；无任何命中返回空应答体。 */
static size_t handle_flash_cmd(uint8_t identity, const uint8_t *req, size_t len,
                               uint8_t subcmd, uint8_t *resp, size_t cap)
{
    if (subcmd != 0x04) {
        return NS2_FRAME_HEADER_LEN;
    }
    /* 请求帧 = 8B 帧头 + 8B magic（rlen + 3B 地址 + 填充），共 16B。 */
    if (len < NS2_FRAME_HEADER_LEN + 8 || cap < NS2_FRAME_HEADER_LEN + 8 + 0x78) {
        return 0;
    }
    const size_t rlen = req[8];
    if (rlen == 0 || rlen > 0x78) {
        return 0;
    }
    mem_map_entry_t map[MEM_MAP_LEN];
    build_mem_map(map, identity);
    const uint32_t addr = ((uint32_t)req[14] << 16) | ((uint32_t)req[13] << 8) | req[12];
    const uint32_t addr_end = addr + rlen;
    memset(&resp[16], 0xFF, rlen);
    bool hit = false;
    for (size_t i = 0; i < MEM_MAP_LEN; i++) {
        const uint32_t block_start = map[i].start;
        const uint32_t block_end = block_start + map[i].len;
        if (block_end < addr || block_start > addr_end) {
            continue;
        }
        hit = true;
        if (map[i].len == 0) {
            continue;
        }
        const uint32_t overlap_start = addr > block_start ? addr : block_start;
        const uint32_t overlap_end = addr_end < block_end ? addr_end : block_end;
        memcpy(&resp[16] + (overlap_start - addr),
               &map[i].data[overlap_start - block_start],
               overlap_end - overlap_start);
    }
    if (!hit) {
        ESP_LOGW(TAG, "flash read 0x%06" PRIx32 " not implemented", addr);
        return 0;
    }
    memcpy(&resp[8], &req[8], 8);
    resp[9] = 0x00;
    return NS2_FRAME_HEADER_LEN + 8 + rlen;
}

/** Command 0x03 初始化与连接建立（controller.md §6.2）。 */
static size_t handle_init_cmd(session_slot_t *ses, const uint8_t *req, size_t len,
                              uint8_t subcmd, uint8_t *resp)
{
    switch (subcmd) {
    case 0x01:
        ESP_LOGI(TAG, "wake advertising request=%u (deferred)", req[8]);
        return NS2_FRAME_HEADER_LEN;
    case 0x0A:
        if (len >= 9 && (req[8] == NS2_REPORT_ID_05 || req[8] == NS2_REPORT_ID_09)) {
            ses->report_format = req[8];
            ESP_LOGI(TAG, "input report format -> 0x%02x", req[8]);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x07:
        /* 直接注入配对信息：6B 主机 MAC（反序）+ 16B LTK（反序）。
         * MAC 按线格式原样存储；LTK 反转回派生形态（0x15/0x04 路径的 A1^B1）。 */
        if (len >= NS2_FRAME_HEADER_LEN + 22) {
            uint8_t ltk[16];
            reverse_bytes(&req[14], ltk, 16);
            ble_creds_save(ses->identity, &req[8], ltk);
            /* 配对交换走完才算确认是 NS2 主机：此时才记回连广播要用的地址。 */
            ble_creds_note_host_mac(ses->identity, &req[8]);
            ses->state = SESSION_NORMAL;
            log_session_normal(ses);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x08:
        ble_creds_clear(ses->identity);
        ses->state = SESSION_CONNECTED_WAIT_PAIR;
        return NS2_FRAME_HEADER_LEN;
    case 0x09:
        /* 凭证在 0x15/0x03 与 0x03/0x07 时即写 NVS，无需额外动作。 */
        return NS2_FRAME_HEADER_LEN;
    case 0x03:
    case 0x0D:
        /* USB 通道/上报初始化应答带 4 字节确认字。 */
        memset(&resp[8], 0, 4);
        return NS2_FRAME_HEADER_LEN + 4;
    default:
        return NS2_FRAME_HEADER_LEN;
    }
}

/** Command 0x09 玩家 LED。 */
static size_t handle_led_cmd(session_slot_t *ses, const uint8_t *req, size_t len, uint8_t subcmd)
{
    switch (subcmd) {
    case 0x01:
        ses->player_leds = 0x01;
        break;
    case 0x02:
        ses->player_leds = 0x02;
        break;
    case 0x03:
        ses->player_leds = 0x04;
        break;
    case 0x04:
        ses->player_leds = 0x08;
        break;
    case 0x05:
        ses->player_leds = 0x0F;
        break;
    case 0x06:
        ses->player_leds = 0x00;
        break;
    case 0x07:
        ses->player_leds = len >= 9 ? (uint8_t)(req[8] & 0x0F) : 0;
        break;
    default:
        ESP_LOGI(TAG, "led subcmd 0x%02x (blink)", subcmd);
        return NS2_FRAME_HEADER_LEN;
    }
    ESP_LOGI(TAG, "player LED mask -> 0x%x", ses->player_leds);
    ns2_output_emit_player_led(ses->player_leds);
    return NS2_FRAME_HEADER_LEN;
}

/** Command 0x0C 特性掩码：body 首字节生效（bit5 触觉影响 0x09 状态标志）。 */
static size_t handle_feature_cmd(session_slot_t *ses, const uint8_t *req, size_t len,
                                 uint8_t subcmd, uint8_t *resp)
{
    const uint8_t mask = len >= 12 ? req[8] : 0;
    switch (subcmd) {
    case 0x01: {
        /* get feature info：4B 保留 + 特性位图（按键/摇杆全支持，IMU 与
         * 未用字段为 Pro 布局，触觉可用），对齐已验证实现。 */
        static const uint8_t feature_info[12] = {
            0x00, 0x00, 0x00, 0x00, 0x07, 0x07, 0x01, 0x01, 0x00, 0x03, 0x00, 0x00,
        };
        memcpy(&resp[8], feature_info, sizeof(feature_info));
        return NS2_FRAME_HEADER_LEN + sizeof(feature_info);
    }
    case 0x02:
        ses->feature_mask = mask;
        break;
    case 0x04:
        /* 启用特性：主机采用输入报文的门槛（参考实现据此进入 DEV_READY 并
         *  开始上报）。 */
        ses->feature_mask |= mask;
        ses->features_enabled = true;
        ESP_LOGI(TAG, "features enabled (mask 0x%02x) -> input reports on", mask);
        break;
    case 0x05:
        ses->feature_mask = (uint8_t)(ses->feature_mask & ~mask);
        ses->features_enabled = false;
        break;
    case 0x06:
        ESP_LOGI(TAG, "feature sampling config 0x%02x (ignored)", mask);
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "feature mask -> 0x%02x (subcmd 0x%02x)", ses->feature_mask, subcmd);
    memset(&resp[8], 0, 4);
    return NS2_FRAME_HEADER_LEN + 4;
}

/** Command 0x15 私有配对四步（controller.md §3）：MAC 交换 -> 公钥交换 ->
 * AES-128-ECB 挑战 -> 确认保存；全程不涉及标准 SMP。请求体以 0x00 前缀、
 * 应答体以 0x01 前缀（实机抓包，ndeadly/switch2_controller_research）。 */
static size_t handle_pairing_cmd(session_slot_t *ses, const uint8_t *req, size_t len,
                                 uint8_t subcmd, uint8_t *resp, size_t cap)
{
    switch (subcmd) {
    case 0x01:
        /* 步骤 1：请求体 `00 [地址数] [地址数×6B 主机地址反序]`（主机发两个地址，
         * 绑定首个）；应答体 `01 04 01 [自身地址反序]`。 */
        if (len < NS2_FRAME_HEADER_LEN + 8 || cap < NS2_FRAME_HEADER_LEN + 9) {
            return 0;
        }
        if (req[9] >= 1) {
            memcpy(ses->pair_host_mac, &req[10], 6);
            ses->pair_mac_ready = true;
        }
        resp[8] = 0x01;
        resp[9] = 0x04;
        resp[10] = 0x01;
        /* 抓包应答的地址即 NimBLE 存储序原样，不做反转。 */
        memcpy(&resp[11], s_ses.own_mac, 6);
        return NS2_FRAME_HEADER_LEN + 9;
    case 0x04:
        /* 步骤 2：请求体 `00 [16B 主机公钥 A1 反序]`；LTK 以反序存储
         * `R(A1) XOR R(B1)`（AES 密钥字节序约定，对齐已验证实现），
         * 应答体 `01 [16B B1]`。 */
        if (len < NS2_FRAME_HEADER_LEN + 17 || cap < NS2_FRAME_HEADER_LEN + 17) {
            return 0;
        }
        for (int i = 0; i < 16; i++) {
            ses->pair_ltk[i] = (uint8_t)(req[9 + (15 - i)] ^ ns2_pair_pubkey_b1[15 - i]);
        }
        ses->pair_ltk_ready = true;
        resp[8] = 0x01;
        memcpy(&resp[9], ns2_pair_pubkey_b1, 16);
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x02:
        /* 步骤 3：请求体 `00 [16B 挑战码 A2 反序]`；
         * B2 = AES128_ECB(Key=存储 LTK, Data=reverse(A2))，应答体
         * `01 [16B B2 原始输出]`（不再反转，对齐已验证实现）。 */
        if (!ses->pair_ltk_ready || len < NS2_FRAME_HEADER_LEN + 17 ||
            cap < NS2_FRAME_HEADER_LEN + 17) {
            return 0;
        }
        {
            uint8_t a2[16];
            uint8_t b2[16];
            reverse_bytes(&req[9], a2, 16);
            if (!aes_ecb_block(ses->pair_ltk, a2, b2)) {
                ESP_LOGE(TAG, "psa aes failed");
                return 0;
            }
            resp[8] = 0x01;
            memcpy(&resp[9], b2, 16);
        }
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x03:
        /* 步骤 4：确认并按身份持久化主机 MAC + LTK，同时注入 NimBLE bonding
         * store（rand/ediv 全 0），主机后续的标准加密请求即可用该 LTK。 */
        if (ses->pair_mac_ready && ses->pair_ltk_ready) {
            ble_creds_save(ses->identity, ses->pair_host_mac, ses->pair_ltk);
            ble_creds_note_host_mac(ses->identity, ses->pair_host_mac);
            inject_ltk_to_ble_store(ses->pair_host_mac, ses->pair_ltk);
            ses->pair_mac_ready = false;
            ses->pair_ltk_ready = false;
            ses->state = SESSION_NORMAL;
            log_session_normal(ses);
        }
        resp[8] = 0x01;
        return NS2_FRAME_HEADER_LEN + 1;
    default:
        ESP_LOGW(TAG, "pairing subcmd 0x%02x unsupported", subcmd);
        return NS2_FRAME_HEADER_LEN;
    }
}

/** 前 n（最多 16）字节的十六进制串，写入调用方缓冲（长度 >= 3*16+4）。 */
static const char *hex_prefix(const uint8_t *data, size_t len, char *out, size_t cap)
{
    const size_t n = len < 16 ? len : 16;
    size_t used = 0;
    for (size_t i = 0; i < n && used + 4 <= cap; i++) {
        const int written = snprintf(&out[used], cap - used, i == 0 ? "%02x" : " %02x", data[i]);
        if (written <= 0 || (size_t)written >= cap - used) {
            break;
        }
        used += (size_t)written;
    }
    if (len > n && used + 4 <= cap) {
        snprintf(&out[used], cap - used, " ..");
    }
    out[cap - 1] = 0;
    return out;
}

void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport,
                            uint16_t conn_handle)
{
    session_slot_t *ses = session_by_conn(conn_handle);
    if (ses == NULL) {
        ESP_LOGW(TAG, "command on unknown conn %u", conn_handle);
        return;
    }
    if (len < NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "command too short (%u)", (unsigned)len);
        return;
    }
    char resp_hex[3 * 16 + 4];
    char rsp_hex[3 * 16 + 4];
    /* 主机初始化与运行期的每一步命令都留痕：真机排查「连上但没输入」时，
     * 对不上抓包的握手步骤一眼可见。应答体同样留前 16 字节，用于对照
     * 主机重复轮询某条命令（重复轮询说明该应答没被主机接受）。 */
    ESP_LOGI(TAG, "cmd 0x%02x/0x%02x (%uB) %s", data[0], data[3], (unsigned)len,
             hex_prefix(data, len, resp_hex, sizeof(resp_hex)));
    const uint8_t cmd = data[0];
    const uint8_t subcmd = data[3];

    uint8_t resp[ANSWER_PREFIX_LEN + 136];
    uint8_t *frame = &resp[ANSWER_PREFIX_LEN];
    memset(resp, 0, ANSWER_PREFIX_LEN);
    size_t resp_len;
    switch (cmd) {
    case 0x07:
        /* 初始握手（§10.2 阶段 1）：应答体 1 字节 0x00。 */
        frame[8] = 0x00;
        resp_len = NS2_FRAME_HEADER_LEN + 1;
        break;
    case NS2_CMD_SPI_FLASH:
        resp_len = handle_flash_cmd(ses->identity, data, len, subcmd, frame,
                                    sizeof(resp) - ANSWER_PREFIX_LEN);
        break;
    case 0x03:
        resp_len = handle_init_cmd(ses, data, len, subcmd, frame);
        break;
    case 0x09:
        resp_len = handle_led_cmd(ses, data, len, subcmd);
        break;
    case 0x0A: {
        const uint8_t sample = subcmd == 0x02 && len >= 9 ? data[8] : subcmd;
        ESP_LOGI(TAG, "haptic sample 0x%02x", sample);
        ns2_output_emit_haptic_sample(sample);
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    }
    case 0x0C:
        resp_len = handle_feature_cmd(ses, data, len, subcmd, frame);
        break;
    case 0x11:
        /* 抓包样例：0x11/0x01 返回 4B 确认字，0x11/0x03 返回 0x1C 传感器块。 */
        if (subcmd == 0x03) {
            static const uint8_t sensor_block[28] = {
                0x01, 0x20, 0x03, 0x00, 0x00, 0x0a, 0xe8, 0x1c,
                0x3b, 0x79, 0x7d, 0x8b, 0x3a, 0x0a, 0xe8, 0x9c,
                0x42, 0x58, 0xa0, 0x0b, 0x42, 0x0a, 0xe8, 0x9c,
                0x41, 0x58, 0xa0, 0x0b,
            };
            memcpy(&frame[8], sensor_block, sizeof(sensor_block));
            resp_len = NS2_FRAME_HEADER_LEN + sizeof(sensor_block);
        } else {
            frame[8] = 0x01;
            memset(&frame[9], 0, 3);
            resp_len = NS2_FRAME_HEADER_LEN + 4;
        }
        break;
    case 0x16:
        /* 未知功能命令（实机抓包：应答体 24 字节 0）。 */
        memset(&frame[8], 0, 24);
        resp_len = NS2_FRAME_HEADER_LEN + 24;
        break;
    case 0x18:
        /* 主机在会话中每约 10 秒轮询一次 0x18/0x01，期望 8 字节应答体
         * （controller.md §6 与已验证实现一致）。不回这个体，主机不会把
         * 这台手柄当成可用输入源——「连上、订阅了、上报也在发，但按键没
         * 反应」正是这个现象。0x18/0x03 只回显请求里的那一字节。 */
        if (subcmd == 0x01) {
            static const uint8_t body[8] = {
                0x00, 0x00, 0x40, 0xF0, 0x00, 0x00, 0x60, 0x00,
            };
            memcpy(&frame[8], body, sizeof(body));
            resp_len = NS2_FRAME_HEADER_LEN + sizeof(body);
        } else if (subcmd == 0x03) {
            frame[8] = len >= 9 ? data[8] : 0x07;
            resp_len = NS2_FRAME_HEADER_LEN + 1;
        } else {
            resp_len = NS2_FRAME_HEADER_LEN;
        }
        break;
    case NS2_CMD_VERSION:
        if (subcmd == 0x01) {
            ns2_body_version(&frame[8], ses->identity);
            resp_len = NS2_FRAME_HEADER_LEN + NS2_VERSION_BODY_LEN;
        } else {
            resp_len = NS2_FRAME_HEADER_LEN;
        }
        break;
    case NS2_CMD_PAIRING:
        resp_len = handle_pairing_cmd(ses, data, len, subcmd, frame,
                                      sizeof(resp) - ANSWER_PREFIX_LEN);
        break;
    default:
        ESP_LOGW(TAG, "unhandled command 0x%02x/0x%02x", cmd, subcmd);
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    }

    if (resp_len == 0) {
        ESP_LOGW(TAG, "cmd 0x%02x/0x%02x response overflow", cmd, subcmd);
        resp_len = NS2_FRAME_HEADER_LEN;
    }
    ns2_frame_response_header(frame, cmd, transport, subcmd);
    ble_controller_notify_answer(conn_handle, resp, ANSWER_PREFIX_LEN + resp_len);
    ESP_LOGI(TAG, "rsp 0x%02x/0x%02x (%uB) %s", cmd, subcmd,
             (unsigned)(ANSWER_PREFIX_LEN + resp_len),
             hex_prefix(resp, ANSWER_PREFIX_LEN + resp_len, rsp_hex, sizeof(rsp_hex)));
}

void ns2_session_on_output(const uint8_t *data, size_t len, uint16_t conn_handle)
{
    /* Output Report 0x02：2x16B LRA 参数包（§5.4）。板卡无震动马达：
     * 解析为结构化震动事件经 ns2_output 分发给监听者（当前记录日志，
     * M5 起转发给 USB 源手柄 / 桥接 PC）。 */
    (void)conn_handle;
    ns2_rumble_event_t event;
    if (!ns2_rumble_parse(data, len, &event)) {
        ESP_LOGW(TAG, "output report too short (%u)", (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "rumble: L=%u R=%u (0x%02x/0x%02x)",
             (unsigned)event.left_on, (unsigned)event.right_on,
             event.raw[0], event.raw[16]);
    ns2_output_emit_rumble(&event);
}

void ns2_session_on_composite(const uint8_t *data, size_t len, uint16_t conn_handle)
{
    /* 实机抓包：复合输出 = 33 字节 0x00 填充 + 命令帧（首字节为震动形态
     * 的 Switch 1 布局未被 Switch 2 主机使用）。 */
    if (len < 33 + NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "composite too short (%u)", (unsigned)len);
        return;
    }
    /* 复合写入的震动段只有一条 16 字节 LRA 参数包（与 0x0012 的左右两条
     * 不同），本工程不模拟马达，遂不解析；震动反馈走 0x0012 通路的
     * ns2_session_on_output。 */
    ns2_session_on_command(&data[33], len - 33, NS2_FRAME_TRANSPORT_BLE, conn_handle);
}

bool ns2_session_rumble_enabled(void)
{
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active && (s_ses.sess[i].feature_mask & 0x20) != 0) {
            return true;
        }
    }
    return false;
}

void ns2_session_start_pairing_mode(void)
{
    s_ses.pairing_mode = true;
    /* 手动配对恒发标准发现广播（目标 MAC 全零，对齐 §12 配对时机语义）：
     * 已配对的主机会把它当作重新配对，未配对的主机可直接首次配对。
     * JoyCon 组合左右两只同时进入发现广播（按下 LR 的组合确认流程）。 */
    resume_advertising();
    ESP_LOGI(TAG, "pairing mode: discovery advertising");
}

void ns2_session_stop_pairing_mode(void)
{
    s_ses.pairing_mode = false;
    if (!ble_controller_connected()) {
        ble_controller_adv_stop();
    }
    ESP_LOGI(TAG, "pairing mode stopped");
}

bool ns2_session_pairing_mode_active(void)
{
    return s_ses.pairing_mode;
}

void ns2_session_press_lr(void)
{
    /* 配对模式期间双身份发现广播已在发（start_pairing_mode 保证），这里只
     * 补 L+R 按键注入：主机 Grip 界面的组合确认动作，双连接时左右两只都
     * 会上报。 */
    dp_plane_debug_key(PAD_BTN_LB | PAD_BTN_RB, 1000);
    ESP_LOGI(TAG, "press LR (mode=%u, pairing=%u)", (unsigned)s_ses.device_mode,
             (unsigned)s_ses.pairing_mode);
}

bool ns2_session_paired(void)
{
    if (s_ses.device_mode == NS2_ID_PRO) {
        return ble_creds_count(NS2_ID_PRO) > 0;
    }
    return ble_creds_count(NS2_ID_JOYCON_L) > 0 && ble_creds_count(NS2_ID_JOYCON_R) > 0;
}

bool ns2_session_host_registered(void)
{
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active && s_ses.sess[i].state == SESSION_NORMAL) {
            return true;
        }
    }
    return false;
}

bool ns2_session_waiting_pair(void)
{
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active && s_ses.sess[i].state == SESSION_CONNECTED_WAIT_PAIR) {
            return true;
        }
    }
    return false;
}

void ns2_session_unpair(void)
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        ble_creds_clear(ids[i]);
    }
    /* 凭证清空后回连广播失效，恢复为未配对发现广播。 */
    resume_advertising();
    ESP_LOGI(TAG, "pairing credentials cleared");
}

/* --- 固件假升级会话：主机经 0x0018 推送升级数据的伪装接收 --- */

static struct {
    bool active;
    uint32_t bytes;
    int64_t last_us;
} s_fwupd;

/** 静默视为传输结束：真实升级流程未逆向（无公开文档），按「收完即成功」
 * 处理——递增上报版本并落盘，主机随后的版本查询即视为已升级到新版本。 */
#define FWUPD_IDLE_TIMEOUT_US (10 * 1000000LL)

void ns2_session_on_fw_upgrade(const uint8_t *data, size_t len)
{
    if (!s_fwupd.active) {
        s_fwupd.active = true;
        s_fwupd.bytes = 0;
        ESP_LOGI(TAG, "fw upgrade session started (masquerade)");
    }
    s_fwupd.bytes += len;
    s_fwupd.last_us = esp_timer_get_time();
}

static void fwupd_finish(void)
{
    uint8_t ver[3];
    memcpy(ver, app_config_get()->fw_version, sizeof(ver));
    if (++ver[2] > 0x63) {
        ver[2] = 0x00;
        if (++ver[1] > 0x63) {
            ver[1] = 0x00;
            ++ver[0];
        }
    }
    app_config_set_fw_version(ver);
    ESP_LOGI(TAG, "fw upgrade finished: %u bytes received, report version -> %u.%u.%u",
             (unsigned)s_fwupd.bytes, ver[0], ver[1], ver[2]);
    s_fwupd.active = false;
}

void ns2_session_tick(void)
{
    if (s_fwupd.active && esp_timer_get_time() - s_fwupd.last_us > FWUPD_IDLE_TIMEOUT_US) {
        fwupd_finish();
    }
    /* 唤醒窗口到时收尾：立刻回到 0x00 的回连广播，别让主机在休眠中反复被叫醒。 */
    if (s_wake_win.until_us != 0 && !wake_burst_active()) {
        ns2_adv_wake_window_close(&s_wake_win);
        ESP_LOGI(TAG, "wake burst finished");
        if (!ble_controller_connected() && !s_ses.pairing_mode) {
            resume_advertising();
        }
    }

    /* 休眠看门狗：已订阅输入但间隔停在激活门槛之上（ns2_adv_dormant_link）
     * 持续 NS2_DORMANT_TICKS 秒的连接，主机永远不会采用它的输入——断开并
     * 开唤醒突发，逼主机按会激活的回连路径重连。手动配对模式下不干预
     * （此时由用户主导流程）。 */
    if (!s_ses.pairing_mode) {
        for (size_t i = 0; i < SESSION_MAX; i++) {
            session_slot_t *ses = &s_ses.sess[i];
            if (!ses->active || ses->state != SESSION_NORMAL) {
                continue;
            }
            const bool subscribed = ble_controller_input_notify_ready(
                ses->conn_handle, ses->report_format);
            if (!subscribed || !ns2_adv_dormant_link(subscribed, ses->features_enabled)) {
                ses->dormant_ticks = 0;
                continue;
            }
            if (++ses->dormant_ticks < NS2_DORMANT_TICKS ||
                s_dormant_drops >= NS2_DORMANT_MAX_DROPS) {
                continue;
            }
            s_dormant_drops++;
            ses->dormant_ticks = 0;
            ESP_LOGW(TAG, "dormant link (conn=%u, features not enabled) -> drop + wake "
                     "(attempt %u/%u)", ses->conn_handle,
                     (unsigned)s_dormant_drops, (unsigned)NS2_DORMANT_MAX_DROPS);
            ns2_session_wake_request();
            break;
        }
    }
}

/* --- 输出会话视图（dp 的输出通道经 sink 间接调用）--- */

size_t ns2_session_output_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active) {
            n++;
        }
    }
    return n;
}

bool ns2_session_output_info(size_t index, uint8_t *identity, uint8_t *report_format)
{
    size_t n = 0;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (!s_ses.sess[i].active) {
            continue;
        }
        if (n == index) {
            *identity = s_ses.sess[i].identity;
            *report_format = s_ses.sess[i].report_format;
            return true;
        }
        n++;
    }
    return false;
}

void ns2_session_deliver_report(size_t index, uint8_t report_id, const uint8_t *body)
{
    size_t n = 0;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (!s_ses.sess[i].active) {
            continue;
        }
        if (n == index) {
            session_slot_t *slot = &s_ses.sess[i];
            const uint16_t conn = slot->conn_handle;
            /* 特性启用（0x0c/0x04）前不发输入通知：对齐参考实现的 DEV_READY
             *  门槛——主机不采用未启用链路上的输入，提前灌报文只会挤占发送
             *  队列（休眠连接上曾实测近半数通知因拥塞失败）。 */
            if (!slot->features_enabled) {
                return;
            }
            if (ble_controller_input_notify_ready(conn, report_id)) {
                slot->reports++;
                if (slot->reports == 1) {
                    ESP_LOGI(TAG, "first input report -> %s (conn=%u, fmt=0x%02x)",
                             ns2_identity_name(slot->identity), conn, report_id);
                }
            }
            if (report_id == NS2_REPORT_ID_05) {
                ble_controller_notify_input_05(conn, body);
            } else {
                ble_controller_notify_input_09(conn, body);
            }
            return;
        }
        n++;
    }
}

/** 控制面下发手柄身份（类型 + 配色）：重建出厂块；host 已同步时立即
 *  重建广播拓扑（Pro 单身份 / JoyCon 双身份），下次广播生效。 */
void ns2_session_set_identity(bool joycon, uint32_t body_rgb,
                              uint32_t button_rgb, uint32_t grip_rgb)
{
    const uint8_t mode = joycon ? (uint8_t)NS2_ID_JOYCON_L : (uint8_t)NS2_ID_PRO;
    const bool changed = s_ses.device_mode != mode ||
                         s_ses.body_color != body_rgb ||
                         s_ses.button_color != button_rgb ||
                         s_ses.grip_color != grip_rgb;
    if (!changed) {
        return;
    }
    s_ses.device_mode = mode;
    s_ses.body_color = body_rgb;
    s_ses.button_color = button_rgb;
    s_ses.grip_color = grip_rgb;
    /* 形态切换会改变「所有身份在线」的含义，重新武装成对在线行。 */
    s_ses.pair_online_logged = false;
    if (s_ses.synced) {
        factory_init();
        if (!ble_controller_connected()) {
            resume_advertising();
        }
    }
    ESP_LOGI(TAG, "controller identity -> %s (body=%06lx btn=%06lx grip=%06lx)",
             joycon ? "joycon-lr" : "pro", (unsigned long)body_rgb,
             (unsigned long)button_rgb, (unsigned long)grip_rgb);
}

/* --- 链路状态视图（串口诊断与控制面经这些接口取数）--- */

size_t ns2_session_mode_identities(uint8_t out[2])
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)ids[i];
    }
    return n;
}

bool ns2_session_identity_mac(uint8_t identity, uint8_t out[6])
{
    return identity_mac(identity, out);
}

uint8_t ns2_session_player_leds(void)
{
    /* 会话槽在连接建立与断开时被整体清零，掩码因此随连接自动复位。 */
    uint8_t mask = 0;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_ses.sess[i].active) {
            mask |= s_ses.sess[i].player_leds;
        }
    }
    return (uint8_t)(mask & 0x0F);
}

bool ns2_session_status(uint8_t identity, ns2_session_status_t *out)
{
    if (out == NULL || !identity_in_mode((ns2_identity_t)identity)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->identity = identity;
    out->creds = (uint8_t)ble_creds_count((ns2_identity_t)identity);
    out->advertising = ble_controller_adv_running(identity);
    out->mac_valid = identity_mac(identity, out->mac);

    const session_slot_t *slot = session_by_identity(identity);
    if (slot == NULL) {
        out->state = out->advertising ? NS2_LINK_ADVERTISING : NS2_LINK_IDLE;
        return true;
    }
    out->connected = true;
    out->conn_handle = slot->conn_handle;
    out->report_format = slot->report_format;
    out->notify_05 = ble_controller_input_notify_ready(slot->conn_handle, NS2_REPORT_ID_05);
    out->notify_09 = ble_controller_input_notify_ready(slot->conn_handle, NS2_REPORT_ID_09);
    out->features_enabled = slot->features_enabled;
    out->reports = slot->reports;
    ble_controller_conn_itvl(slot->conn_handle, &out->conn_itvl);
    out->state = slot->state == SESSION_NORMAL ? NS2_LINK_NORMAL : NS2_LINK_WAIT_PAIR;
    return true;
}

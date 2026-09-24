#include "ble_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_store.h"
#include "psa/crypto.h"

#include "app_config.h"
#include "ble_controller.h"
#include "ble_creds.h"
#include "dp_plane.h"
#include "ns2_adv.h"
#include "ns2_identity.h"
#include "ns2_frames.h"
#include "ns2_nfc.h"
#include "ns2_output.h"
#include "ns2_report.h"
#include "ns2_serial.h"
#include "ns2_upgrade.h"
#include "pad_state.h"

static const char *TAG = "remapad_blses";

/** 假升级的收尾动作（定义在文件末尾的假升级会话段）。 */
static void fwupd_schedule_apply(void);

#define ANSWER_PREFIX_LEN 14
#define FACTORY_SIZE 2048u

/** 会话状态：休眠/唤醒广播顺延到后续里程碑。 */
typedef enum {
    SESSION_ADV_DISCOVERY = 0,
    SESSION_ADV_RECONNECT,
    SESSION_CONNECTED_WAIT_PAIR,
    SESSION_NORMAL,
} session_state_t;

/** 单条连接的会话状态（一台主机一条，第二槽留给重连过渡）。 */
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
    /** 注册证据（见 ns2_adv_host_registered）：连接时对端地址命中凭证。 */
    bool addr_matched;
    /** 注册证据：私有配对握手走完（0x15/0x03 或 0x03/0x07）。 */
    bool pair_handshake_done;
} session_slot_t;

static struct {
    session_slot_t sess[SESSION_MAX];
    uint8_t own_mac[6];
    bool synced;
    /** 四段配色：机身 / 按键 / 高光 / 握把（0 = 未配置，出厂块按默认值填），
     *  随用户设置落盘。 */
    uint32_t body_color;
    uint32_t button_color;
    uint32_t accent_color;
    uint32_t grip_color;
    bool pairing_mode;
    /** 身份在线行是否已打印过（断连后重新武装）。 */
    bool pair_online_logged;
} s_ses;

/** 当前手柄身份集合：设备只模拟 Pro Controller 2，因此只有一个身份。
 *  一台控制器只有一个 public 地址，主机也只接受 public 地址的广播。 */
static size_t mode_identities(ns2_identity_t *out)
{
    out[0] = NS2_ID_PRO;
    return 1;
}

/** 对账开关：广播地址形态（ns2_adv_addr_form_t），默认 auto。不落盘，
 *  只影响本轮广播——用于分辨主机是否按地址形态（public / 静态随机）过滤
 *  广播：手柄一律用 public 地址，本机派生的都是静态随机地址。 */
static uint8_t s_adv_addr_form;

/** 是否用公共伪装地址广播：auto 与 public 都用公共地址——主机的芯片过滤只
 *  接受 public 地址的广播，派生的静态随机地址在主机侧完全看不见，
 *  因此派生形态只留作对账开关。 */
static bool adv_uses_public_addr(void)
{
    return s_adv_addr_form != NS2_ADV_ADDR_RANDOM;
}

/** 这一轮真正用的广播地址；NULL 表示用公共伪装地址（传输层语义）。 */
static const uint8_t *adv_addr_for(void)
{
    static uint8_t s_addr[6];
    if (!s_ses.synced || adv_uses_public_addr()) {
        return NULL;
    }
    ns2_identity_adv_addr_random(s_ses.own_mac, s_addr);
    return s_addr;
}

/** Pro Controller 2 的 PID。 */
#define NS2_PRO_PID 0x2069u

/** Pro Controller 2 的序列号（3 字母前缀 + 10 位数字，末位校验位由 ns2_serial_build 补齐）。 */
#define NS2_PRO_SERIAL_PREFIX "HEJ"
#define NS2_PRO_SERIAL_DIGITS "7100112345"

static uint8_t s_factory[FACTORY_SIZE];

/** 0x13000 出厂数据块（布局）：`01 00` + 序列号@2 + `00 00`
 * + VID/PID@18 + 版本@22 + 机身配色@25，尾部 0xFF。按身份分槽存放，
 * 指令处理按连接身份取用。 */
#define MEM_FACTORY_MAX NS2_ID_COUNT
static uint8_t s_mem_factory[MEM_FACTORY_MAX][64];

/** 0x7E40 首块（主机初始化经 0x02/0x04 读取）：6B 头 + 14B
 * 序列号@6 + 2B 保留@20 + 4B VID/PID@22 + 3B 版本@26 + 12B 配色@29。 */
static uint8_t s_mem_7e40[MEM_FACTORY_MAX][64];

/** 0x13080 / 0x130C0 摇杆校准块（64B）。 */
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

/** 帧内字节序列整体反转（MAC、AES 挑战与注入 LTK 的字节序变换）。 */
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

/** 出厂数据区（序列号、VID/PID、机身配色与摇杆校准）。
 * 校准取中位 2048、行程 ±2047/2048，与编码器 0-4095 直发语义保持 1:1。
 * 序列号、版本与配色按当前模式的每个身份各生成一份。 */
static void factory_init(void)
{
    memset(s_factory, 0xFF, sizeof(s_factory));
    static const uint8_t stick_cal[9] = {0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80};
    memcpy(&s_factory[0x00A8], stick_cal, sizeof(stick_cal));
    memcpy(&s_factory[0x00E8], stick_cal, sizeof(stick_cal));

    const uint8_t *ver = app_config_get()->fw_version;
    /* 四段配色（0x13019 机身 / 0x1301C 按键 / 0x1301F 高光 / 0x13022 握把）：
     * 未配置的段取 Pro Controller 2 的原厂值（机身深灰、按键浅灰、高光近白、
     * 握把深灰），界面上选配色时四段一起覆盖。 */
    const uint32_t body = s_ses.body_color != 0 ? s_ses.body_color : 0x232323u;
    const uint32_t button = s_ses.button_color != 0 ? s_ses.button_color : 0xA0A0A0u;
    const uint32_t accent = s_ses.accent_color != 0 ? s_ses.accent_color : 0xE6E6E6u;
    const uint32_t grip = s_ses.grip_color != 0 ? s_ses.grip_color : 0x323232u;
    const uint8_t colors[12] = {
        (uint8_t)(body >> 16), (uint8_t)(body >> 8), (uint8_t)(body),
        (uint8_t)(button >> 16), (uint8_t)(button >> 8), (uint8_t)(button),
        (uint8_t)(accent >> 16), (uint8_t)(accent >> 8), (uint8_t)(accent),
        (uint8_t)(grip >> 16), (uint8_t)(grip >> 8), (uint8_t)(grip),
    };
    char serial[15];
    ns2_serial_build(NS2_PRO_SERIAL_PREFIX, NS2_PRO_SERIAL_DIGITS, serial);

    /* 0x7E40 首块：6B 头内容未验证，取 `01 00` 前缀填零。
     * 此前未提供该块时主机初始化读取失败，可能是固件更新
     * 提示的诱因（版本读不到即视为旧固件）。 */
    uint8_t *blk = s_mem_7e40[NS2_ID_PRO];
    memset(blk, 0xFF, 64);
    blk[0] = 0x01;
    blk[1] = 0x00;
    memcpy(&blk[6], serial, 14);
    blk[22] = 0x7E;
    blk[23] = 0x05;
    blk[24] = (uint8_t)(NS2_PRO_PID & 0xFF);
    blk[25] = (uint8_t)(NS2_PRO_PID >> 8);
    blk[26] = ver[0];
    blk[27] = ver[1];
    blk[28] = ver[2];
    memcpy(&blk[29], colors, sizeof(colors));

    uint8_t *mem = s_mem_factory[NS2_ID_PRO];
    memset(mem, 0xFF, 64);
    mem[0] = 0x01;
    mem[1] = 0x00;
    memcpy(&mem[2], serial, 14);
    mem[18] = 0x7E;
    mem[19] = 0x05;
    mem[20] = (uint8_t)(NS2_PRO_PID & 0xFF);
    mem[21] = (uint8_t)(NS2_PRO_PID >> 8);
    mem[22] = ver[0];
    mem[23] = ver[1];
    mem[24] = ver[2];
    memcpy(&mem[25], colors, sizeof(colors));
    ESP_LOGI(TAG, "factory serial=%s pid=0x%04x fw=%u.%u.%u", serial, NS2_PRO_PID,
             ver[0], ver[1], ver[2]);
}

/** 休眠看门狗：已订阅输入但主机始终没发 0x0c/0x04（启用特性）持续这么多
 *  秒（tick 每 1 秒一次），判定为主机不采用输入的休眠连接——握把页的快捷
 *  回连正是这个形态（itvl=4 但未启用的链路按键同样无效）。正常握手
 *  在订阅前后一两秒内就会启用特性，15 秒足够宽。 */
#define NS2_DORMANT_TICKS 15

/** 每次上电允许的休眠断开次数上限：超过即放弃（避免与主机反复互相拉扯）。 */
#define NS2_DORMANT_MAX_DROPS 3

/** 本次上电已执行的休眠断开次数（上限 NS2_DORMANT_MAX_DROPS）。 */
static uint8_t s_dormant_drops;

/** 广播窗口：各情况按组装信号开窗（连接键与开机/Dock 的信号搜索带唤醒突发、
 *  断连回连与休眠看门狗重连只发回连形态、HOME 唤醒窗口全程唤醒），主机连上
 *  或窗口到期收窗。设备只在窗口开着或配对流程里广播——不被请求就静默。 */
static ns2_adv_window_t s_adv_win;
static int64_t s_pairing_until_us;
static bool s_user_explicit_disconnect;
static ns2_adv_mode_t s_last_applied_adv_mode = NS2_ADV_OFF;

/** 窗口内形态的对账开关（串口 `adv auto|wake|reconnect`）：钉住一种形态
 *  做 A/B 对账，auto 时按窗口来源决策。 */
static ns2_window_form_t s_window_form = NS2_WINDOW_FORM_AUTO;

/** 起栈意图：栈关着时用户按下的键先记下来，由控制面服务任务起栈、
 *  在同步回调里结算。 */
typedef enum {
    NS2_BLE_INTENT_NONE = 0,
    NS2_BLE_INTENT_CONNECT,
    NS2_BLE_INTENT_PAIRING,
    NS2_BLE_INTENT_WAKE,
} ns2_ble_intent_t;

/** 栈状态：命令回调、同步回调与控制面服务任务之间交接。 */
static portMUX_TYPE s_ble_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t intent; /**< ns2_ble_intent_t：待结算的起栈意图。 */
    bool synced;    /**< 本轮同步回调已跑过：同步前的静默不算「可以关栈」。 */
} s_ble;

/** 记下起栈意图（任意任务上下文）。 */
static void ble_intent_set(ns2_ble_intent_t intent)
{
    portENTER_CRITICAL(&s_ble_mux);
    s_ble.intent = (uint8_t)intent;
    portEXIT_CRITICAL(&s_ble_mux);
}

/** 取走起栈意图（同步回调结算用）。 */
static ns2_ble_intent_t ble_intent_take(void)
{
    portENTER_CRITICAL(&s_ble_mux);
    const ns2_ble_intent_t intent = (ns2_ble_intent_t)s_ble.intent;
    s_ble.intent = (uint8_t)NS2_BLE_INTENT_NONE;
    portEXIT_CRITICAL(&s_ble_mux);
    return intent;
}

/** 是否有待结算的起栈意图。 */
static bool ble_intent_pending(void)
{
    portENTER_CRITICAL(&s_ble_mux);
    const bool pending = s_ble.intent != (uint8_t)NS2_BLE_INTENT_NONE;
    portEXIT_CRITICAL(&s_ble_mux);
    return pending;
}

/** 标记本轮同步已完成。 */
static void ble_synced_set(bool synced)
{
    portENTER_CRITICAL(&s_ble_mux);
    s_ble.synced = synced;
    portEXIT_CRITICAL(&s_ble_mux);
}

/** 本轮同步是否已完成（服务任务据此避开「刚起来还没同步」的窗口）。 */
static bool ble_synced_get(void)
{
    portENTER_CRITICAL(&s_ble_mux);
    const bool synced = s_ble.synced;
    portEXIT_CRITICAL(&s_ble_mux);
    return synced;
}

/** 栈没起来时把用户意图记下来（控制面服务任务据此起栈）；返回是否已记账。 */
static bool ble_stack_defer(ns2_ble_intent_t intent)
{
    if (ble_controller_running()) {
        return false;
    }
    ESP_LOGI(TAG, "ble stack down: intent %u deferred", (unsigned)intent);
    ble_intent_set(intent);
    return true;
}

/** 静默持续时间门槛（微秒）：静默先记时、持续够久才关栈——刚被用户打开的
 *  窗口不会在同一轮里被关掉（窗口与配对流程都跨任务写，单次读数不作数）。 */
#define NS2_BLE_IDLE_GRACE_US (1000000LL)

/** 静默起点（微秒，0 = 不在静默）：由 1 秒周期任务维护，控制面服务任务读。 */
static int64_t s_idle_since_us;

/** 对账开关形态的日志名。 */
static const char *window_form_name(ns2_window_form_t form)
{
    switch (form) {
    case NS2_WINDOW_FORM_WAKE:
        return "wake (0x81)";
    case NS2_WINDOW_FORM_RECONNECT:
        return "reconnect (0x00)";
    default:
        return "auto (window request)";
    }
}

/** 身份的广播形态：配对流程与未配对身份发发现广播，窗口内按请求与凭证决定
 *  回连或唤醒，没有窗口就静默。地址的挑选规则见 ns2_adv_choose_host_mac。 */
static ns2_adv_mode_t adv_mode_for(ns2_identity_t identity, const uint8_t **out_mac)
{
    static uint8_t s_adv_host_mac[6];
    *out_mac = NULL;
    ns2_adv_mode_t mode = ns2_adv_choose_mode(ns2_session_paired(), s_ses.pairing_mode,
                                              &s_adv_win, esp_timer_get_time());
    /* 对账开关只钉窗口内的已配对形态（串口 adv）：连接窗口默认回连 0x00，
     * 对账时可强发唤醒 0x81 比较主机反应。 */
    if (s_window_form != NS2_WINDOW_FORM_AUTO &&
        (mode == NS2_ADV_RECONNECT || mode == NS2_ADV_WAKE)) {
        mode = s_window_form == NS2_WINDOW_FORM_WAKE ? NS2_ADV_WAKE : NS2_ADV_RECONNECT;
    }
    if (mode != NS2_ADV_RECONNECT && mode != NS2_ADV_WAKE) {
        return mode;
    }
    /* 记录值只在「对端命中凭证」的连接与配对交换里写入（普通 BLE 主机不写），
     * 凭证作兜底：配好还没连过时只有凭证地址可用。挑选规则在 ns2_adv。 */
    uint8_t recorded[NS2_CREDS_MAC_LEN];
    const uint8_t *recorded_ptr =
        ble_creds_host_mac(identity, recorded) ? recorded : NULL;
    const uint8_t *creds[NS2_CREDS_MAX];
    const size_t count = ble_creds_count(identity);
    size_t cred_count = 0;
    for (size_t i = count; i > 0; i--) {
        const ns2_cred_record_t *rec = ble_creds_get(identity, i - 1);
        if (rec != NULL && cred_count < NS2_CREDS_MAX) {
            creds[cred_count++] = rec->mac;
        }
    }
    const uint8_t *picked = ns2_adv_choose_host_mac(recorded_ptr, creds, cred_count);
    if (picked == NULL) {
        /* 形态已配齐但这一只没有可用地址：退回发现广播，绝不发全零地址的
         * 回连或唤醒广播（主机既不会回连也不会醒）。 */
        return NS2_ADV_DISCOVERY;
    }
    memcpy(s_adv_host_mac, picked, sizeof(s_adv_host_mac));
    *out_mac = s_adv_host_mac;
    return mode;
}

static const char *adv_mode_name(ns2_adv_mode_t mode)
{
    switch (mode) {
    case NS2_ADV_WAKE:
        return "wake";
    case NS2_ADV_RECONNECT:
        return "reconnect";
    case NS2_ADV_DISCOVERY:
        return "discovery";
    default:
        return "off";
    }
}

/** 启动一个身份的广播：每个身份一个实例（设备只有 Pro 一个身份，PDU 形态见
 *  ble_controller）。地址形态由 adv_addr_for 决定（NULL = 公共伪装地址）。 */
static void adv_start_identity(size_t index, ns2_identity_t identity,
                               const uint8_t payload[NS2_ADV_PAYLOAD_LEN])
{
    ble_controller_adv_start((uint8_t)index, identity, payload, adv_addr_for());
}

/** 按当前状态把广播设成该发的样子：逐身份取形态（静默 / 发现 / 窗口形态），
 *  静默的身份停掉自己的广播实例——设备不被请求连接时不留任何实例在发。
 *  当前形态每个身份只占一个实例（PDU 形态见 ble_controller）。 */
static void apply_advertising(void)
{
    /* 栈关着（静默省电）时没有广播可设：起栈后由同步回调按意图重设。 */
    if (!ble_controller_running()) {
        return;
    }
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        uint8_t adv[NS2_ADV_PAYLOAD_LEN];
        const uint8_t *mac = NULL;
        const ns2_adv_mode_t mode = adv_mode_for(ids[i], &mac);
        s_last_applied_adv_mode = mode;
        if (mode == NS2_ADV_OFF) {
            if (ble_controller_adv_running(ids[i])) {
                ble_controller_adv_stop_identity(ids[i]);
                ESP_LOGI(TAG, "silent: identity %u advertising stopped",
                         (unsigned)ids[i]);
            }
            continue;
        }
        ns2_adv_payload(adv, NS2_PRO_PID, mode, mac);
        adv_start_identity(i, ids[i], adv);
        ESP_LOGI(TAG, "advertising: identity %u %s (%u creds)",
                 (unsigned)ids[i], adv_mode_name(mode), (unsigned)ble_creds_count(ids[i]));
    }
}

void ns2_session_connect(void)
{
    if (ble_stack_defer(NS2_BLE_INTENT_CONNECT)) {
        return;
    }
    if (ble_controller_connected()) {
        ESP_LOGI(TAG, "connect ignored (link in use)");
        return;
    }
    if (s_ses.pairing_mode) {
        ESP_LOGI(TAG, "connect ignored (pairing flow advertising)");
        return;
    }
    /* 用户主动发起的一次连接尝试：休眠看门狗的断开预算跟着复位。 */
    s_dormant_drops = 0;
    if (!ns2_session_paired()) {
        ESP_LOGI(TAG, "connect: no credentials -> pairing flow (discovery advertising)");
        ns2_session_start_pairing_mode();
        return;
    }
    ns2_adv_window_open(&s_adv_win, &NS2_ADV_SIGNAL_SEARCH, esp_timer_get_time());
    ESP_LOGI(TAG, "connect: advertising for %llds",
             (long long)(NS2_ADV_CONNECT_WINDOW_US / 1000000LL));
    apply_advertising();
}

void ns2_session_disconnect(void)
{
    /* 停止广播：收掉窗口与配对流程，链路在线就断开。断连回调据此同步广播，
     * 没有窗口与配对流程就是静默。 */
    ns2_adv_window_close(&s_adv_win);
    s_ses.pairing_mode = false;
    s_pairing_until_us = 0;
    if (ble_controller_connected()) {
        s_user_explicit_disconnect = true;
        ESP_LOGI(TAG, "disconnect: dropping the current link");
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
        return;
    }
    apply_advertising();
    ESP_LOGI(TAG, "disconnect: silent until the next connect request");
}

bool ns2_session_advertising(void)
{
    return ns2_adv_window_active(&s_adv_win, esp_timer_get_time()) || s_ses.pairing_mode;
}

void ns2_session_wake_request(void)
{
    if (ble_stack_defer(NS2_BLE_INTENT_WAKE)) {
        return;
    }
    if (s_ses.pairing_mode) {
        ESP_LOGI(TAG, "wake request ignored (pairing flow)");
        return;
    }
    /* 唤醒是显式请求：开窗发唤醒形态 0x81（窗口到期即静默，主机随后睡下不会
     * 再被叫醒）；已连接就断开，让主机按唤醒广播重新连上来（从握把/顺序页
     * 连上来的会话不采用输入报文，靠这次重连纠正）。 */
    ns2_adv_window_open(&s_adv_win, &NS2_ADV_SIGNAL_WAKE, esp_timer_get_time());
    if (ble_controller_connected()) {
        ESP_LOGI(TAG, "wake: dropping current link to force a reconnect");
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
        return;
    }
    ESP_LOGI(TAG, "wake: wake advertising for %llds",
             (long long)(NS2_ADV_WAKE_WINDOW_US / 1000000LL));
    apply_advertising();
}

void ns2_session_on_sync(const uint8_t own_mac[6])
{
    factory_init();
    s_ses.synced = true;
    memcpy(s_ses.own_mac, own_mac, 6);
    ESP_LOGI(TAG, "host synced, own MAC %02x:%02x:%02x:%02x:%02x:%02x",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
    ble_synced_set(true);
    /* 起栈意图按用户按下的那个键结算；开机没有意图，自动触发 30 秒信号搜索
     * （类似手柄搜索模式，已配对则唤醒+回连，未配对则发发现广播），
     * 超时未建立连接即关栈静默省电。 */
    switch (ble_intent_take()) {
    case NS2_BLE_INTENT_CONNECT:
        ns2_session_connect();
        return;
    case NS2_BLE_INTENT_PAIRING:
        ns2_session_start_pairing_mode();
        return;
    case NS2_BLE_INTENT_WAKE:
        ns2_session_wake_request();
        return;
    default:
        break;
    }
    ns2_session_connect();
}

bool ns2_session_stack_idle(void)
{
    return ns2_adv_stack_idle(ble_controller_connected(), s_ses.pairing_mode,
                              ns2_adv_window_active(&s_adv_win, esp_timer_get_time()));
}

void ns2_session_ble_service(void)
{
    if (!ble_controller_running()) {
        if (!ble_intent_pending()) {
            return;
        }
        ESP_LOGI(TAG, "ble stack start on demand");
        const esp_err_t err = ble_controller_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ble stack start failed: %s", esp_err_to_name(err));
            ble_intent_set(NS2_BLE_INTENT_NONE);
        }
        return;
    }
    /* 意图结算完（同步回调跑过）之前不动栈：刚起来的栈在同步前也是「静默」的。 */
    if (ble_intent_pending() || !ble_synced_get()) {
        return;
    }
    if (!ns2_session_stack_idle()) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_idle_since_us == 0 || now - s_idle_since_us < NS2_BLE_IDLE_GRACE_US) {
        return;
    }
    ESP_LOGI(TAG, "ble stack idle: shutting the controller down (power save)");
    const esp_err_t err = ble_controller_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble stack stop failed: %s", esp_err_to_name(err));
        return;
    }
    s_idle_since_us = 0;
    ble_synced_set(false);
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

/**
 * 注册证据到齐就把会话从等待态提升为已注册（判据见 ns2_adv_host_registered）。
 * 三条证据分别在连接时、配对握手上、以及主机启用特性时出现，三条都走这里——
 * 只认地址的那条会让「主机换了随机地址」或「主机不再重跑 0x15」的链路一直停在
 * 等待态：屏幕停在「配对中…」，配新主机的流程也退不出来。
 */
static void promote_if_host_registered(session_slot_t *ses)
{
    if (ses->state != SESSION_CONNECTED_WAIT_PAIR) {
        return;
    }
    if (!ns2_adv_host_registered(ses->addr_matched, ses->pair_handshake_done,
                                ses->features_enabled)) {
        return;
    }
    ses->state = SESSION_NORMAL;
    log_session_normal(ses);
}

/** 身份在线时打印一次（断连后重新武装）：串口对账时确认会话已建立。 */
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
    ESP_LOGI(TAG, "identity online: %s", ns2_identity_name(ids[0]));
}

/** 身份对外广播地址（NimBLE 存储序）；host 未同步时地址尚未确定。 */
static bool identity_mac(uint8_t identity, uint8_t out[6])
{
    if (!s_ses.synced) {
        return false;
    }
    const uint8_t *addr = adv_addr_for();
    memcpy(out, addr != NULL ? addr : s_ses.own_mac, 6);
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
        /* 研究仓库的 .ltk（用于解密链路）恰是反序形态：按原样写入。 */
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

void ns2_session_set_window_form(ns2_window_form_t form)
{
    /* 只在这里钉窗口内的形态：发现与静默由凭证、配对流程与窗口有效期决定。 */
    s_window_form = form;
    ESP_LOGI(TAG, "window form -> %s", window_form_name(s_window_form));
    if (!ble_controller_connected() && ns2_session_advertising()) {
        apply_advertising();
    }
}

ns2_window_form_t ns2_session_window_form(void)
{
    return s_window_form;
}

bool ns2_session_set_adv_addr_form(uint8_t form)
{
    if (form > NS2_ADV_ADDR_RANDOM) {
        return false;
    }
    s_adv_addr_form = form;
    ESP_LOGW(TAG, "adv addr form -> %s (own %02x:%02x:%02x:%02x:%02x:%02x)",
             ns2_adv_addr_form_name(form),
             s_ses.own_mac[0], s_ses.own_mac[1], s_ses.own_mac[2], s_ses.own_mac[3],
             s_ses.own_mac[4], s_ses.own_mac[5]);
    /* 没连接又确实在广播（连接窗口 / 配对流程）时立刻按新形态重发；已连接
     * 的链路要断开重连才会换地址，改完用 `drop` 或 `pairing start` 触发。 */
    if (!ble_controller_connected() && ns2_session_advertising()) {
        apply_advertising();
    }
    return true;
}

uint8_t ns2_session_adv_addr_form(void)
{
    return s_adv_addr_form;
}

void ns2_session_set_adv_pdu_form(uint8_t form)
{
    ble_controller_set_adv_pdu_form(form);
    if (!ble_controller_connected() && ns2_session_advertising()) {
        apply_advertising();
    }
}

void ns2_session_on_connect(uint16_t conn_handle, uint8_t identity)
{
    session_slot_t *slot = NULL;

    /* 主机已经连上：收掉广播窗口（连接请求达成 / 唤醒达成）。主机随后睡下
     * （链路断开）时不再发信号，那时窗口必须已经关闭，否则会把它重新叫起来。 */
    if (ns2_adv_window_active(&s_adv_win, esp_timer_get_time())) {
        ESP_LOGI(TAG, "advertising window closed (host connected)");
        ns2_adv_window_close(&s_adv_win);
    }
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
    /* 专用输入通道上的报文体固定 0x09：主机不会下发「选择输入报告格式」
     * （0x03/0x0A）的命令，一台主机只暴露自己型号那一种报文。 */
    slot->report_format = NS2_REPORT_ID_09;

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
    slot->addr_matched = matched != NULL;
    slot->state = SESSION_CONNECTED_WAIT_PAIR;
    slot->last_activity_us = esp_timer_get_time();
    if (matched != NULL) {
        /* 回连：把 NVS 里的 LTK 重新注入本周期 NimBLE RAM store。 */
        inject_ltk_to_ble_store(matched->mac, matched->ltk);
    }
    promote_if_host_registered(slot);
    ESP_LOGI(TAG, "waiting host init sequence");
    log_pair_online();
}

void ns2_session_on_connect_fail(void)
{
    if (!ble_controller_connected()) {
        apply_advertising();
    }
}

void ns2_session_on_disconnect(uint16_t conn_handle, uint8_t identity)
{
    session_slot_t *slot = session_by_conn(conn_handle);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    /* 在线状态被打破，下次连上时再打印一次。 */
    s_ses.pair_online_logged = false;
    if (s_user_explicit_disconnect) {
        s_user_explicit_disconnect = false;
        ESP_LOGI(TAG, "disconnected (user requested): silent");
        apply_advertising();
        return;
    }
    /* 主机断开连接（主机休眠或移开）：组装断连回连信号开窗——30 秒内只发
     * 0x00 回连形态、不带唤醒突发，链路断开可能正是用户主动休眠主机，回连
     * 不得把它立刻叫起来；30 秒未重新连上则彻底关闭发射进入静默，需要用户
     * 主动重开。 */
    if (!s_ses.pairing_mode) {
        const int64_t now = esp_timer_get_time();
        ns2_adv_window_open(&s_adv_win, &NS2_ADV_SIGNAL_RECONNECT, now);
        ESP_LOGI(TAG, "disconnected (conn=%u, identity=%s): opening 30s reconnect window",
                 conn_handle, ns2_identity_name(identity));
    }
    apply_advertising();
}

/** SPI 模拟内存映射块。0x13040 与 0x13100 为固定内容；0x13060 与
 * 用户自定义校准区（0x1FC000 运动 / 0x1FC040 主摇杆 / 0x1FC060 副摇杆）
 * 未经写入即未初始化，长度 0、读出为全 0xFF。出厂数据块按当前连接的
 * 身份提供（序列号 / PID / 配色）。 */
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

/** Command 0x03 初始化与连接建立。 */
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
            ses->pair_handshake_done = true;
            promote_if_host_registered(ses);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x08:
        ble_creds_clear(ses->identity);
        /* 主机要求解除配对：回到等待态，等它重新走一遍握手再算注册。 */
        ses->addr_matched = false;
        ses->pair_handshake_done = false;
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
         *  开始上报）。主机只对它已经认下的手柄做这一步，因此这也是一条注册
         *  证据：主机换了随机地址、或主机已有本机凭证而不再重跑 0x15 时，光靠
         *  地址匹配会把在用的链路一直留在等待态。 */
        ses->feature_mask |= mask;
        ses->features_enabled = true;
        promote_if_host_registered(ses);
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

/** Command 0x15 私有配对四步：MAC 交换 -> 公钥交换 ->
 * AES-128-ECB 挑战 -> 确认保存；全程不涉及标准 SMP。请求体以 0x00 前缀、
 * 应答体以 0x01 前缀（ndeadly/switch2_controller_research）。 */
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
        /* 应答里的地址即 NimBLE 存储序原样，不做反转。 */
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
            ses->pair_handshake_done = true;
            promote_if_host_registered(ses);
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

/** 按帧 16 字节一行留痕一段字节（上限 max），供升级等协议的对账。 */
static void log_hex_block(const char *what, const uint8_t *data, size_t len, size_t max)
{
    const size_t n = len < max ? len : max;
    char line[3 * 16 + 4];
    for (size_t off = 0; off < n; off += 16) {
        const size_t chunk = (n - off) < 16 ? (n - off) : 16;
        ESP_LOGI(TAG, "%s+%03u %s", what, (unsigned)off,
                 hex_prefix(&data[off], chunk, line, sizeof(line)));
    }
    if (len > n) {
        ESP_LOGI(TAG, "%s .. %u more bytes", what, (unsigned)(len - n));
    }
}

/** 运行期热路径命令：日志按秒聚合，不做逐包留痕（见 ns2_session_on_command）。 */
static bool cmd_is_hot_path(uint8_t cmd)
{
    return cmd == 0x0A; /* 触觉采样 */
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
    /* 主机初始化与运行期的每一步命令都留痕：排查「连上但没输入」时，
     * 对不上的握手步骤一眼可见。应答体同样留前 16 字节，用于对照
     * 主机重复轮询某条命令（重复轮询说明该应答没被主机接受）。
     * 例外是 0x0A 触觉采样：运行期热路径（游戏里接近输入上报的频率），
     * 逐包日志会把发送环灌满、拖住输入通知，只按
     * 秒聚合。 */
    const bool hot_path = cmd_is_hot_path(data[0]);
    if (!hot_path) {
        ESP_LOGI(TAG, "cmd 0x%02x/0x%02x (%uB) %s", data[0], data[3], (unsigned)len,
                 hex_prefix(data, len, resp_hex, sizeof(resp_hex)));
        if (len > 16) {
            /* 长指令（升级推送若走指令通道就是这种形态）逐字节留痕。 */
            log_hex_block("cmd", data, len, 128);
        }
    }
    const uint8_t cmd = data[0];
    const uint8_t subcmd = data[3];

    uint8_t resp[ANSWER_PREFIX_LEN + 136];
    uint8_t *frame = &resp[ANSWER_PREFIX_LEN];
    memset(resp, 0, ANSWER_PREFIX_LEN);
    size_t resp_len;
    switch (cmd) {
    case 0x01:
        /* NFC 命令通路（协议见 docs/controller-switch2.md）：
         * 软件模拟的 NTAG215 标签，镜像由 amiibo 存储层预置。 */
        resp_len = ns2_nfc_on_command(data, len, subcmd, frame,
                                      sizeof(resp) - ANSWER_PREFIX_LEN);
        break;
    case 0x07:
        /* 初始握手（阶段 1）：应答体 1 字节 0x00。 */
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
        uint8_t sample = 0;
        /* 触觉采样按秒聚合（热路径，见上面的 cmd 留痕例外）：逐包三条日志
         * 在发送环吃紧时会把 NimBLE 主机任务拖住，输入通知随之停摆。 */
        static int64_t s_haptic_log_us;
        static uint32_t s_haptic_count;
        s_haptic_count++;
        const int64_t now_us = esp_timer_get_time();
        if (ns2_haptic_sample_parse(data, len, &sample)) {
            if (now_us - s_haptic_log_us >= 1000000LL) {
                ESP_LOGI(TAG, "haptic x%u/s: sample 0x%02x", (unsigned)s_haptic_count, sample);
                s_haptic_log_us = now_us;
                s_haptic_count = 0;
            }
            ns2_output_emit_haptic_sample(sample);
        }
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    }
    case 0x0C:
        resp_len = handle_feature_cmd(ses, data, len, subcmd, frame);
        break;
    case 0x0D:
        /* 手柄固件更新流程：0x01 进入、0x02/0x03 参数、0x04 数据帧
         * （经 0x0018 分记录推送，由假升级会话应答）、0x05/0x06 收尾与校验、
         * 0x07 应用。主机接受空应答体；0x07 之后主机在等控制器带着新固件回来，
         * 这里补一次伪装重启。 */
        if (subcmd == 0x07) {
            fwupd_schedule_apply();
        }
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    case 0x11:
        /* 0x11/0x01 返回 4B 确认字，0x11/0x03 返回 0x1C 传感器块。 */
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
        /* 未知功能命令（应答体 24 字节 0）。 */
        memset(&frame[8], 0, 24);
        resp_len = NS2_FRAME_HEADER_LEN + 24;
        break;
    case 0x13:
        /* 0x13/0x01：回空体时主机不发 0x0c/0x04、也不订阅输入通道；回 4 字节
         *  `01 00 00 00` 后主机立刻启用特性并开始收输入。
         *  语义与长度未知，`01 00 00 00` 是能走通的形态。 */
        {
            static const uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
            memcpy(&frame[8], body, sizeof(body));
            resp_len = NS2_FRAME_HEADER_LEN + sizeof(body);
        }
        break;
    case 0x18:
        /* 主机在会话中每约 10 秒轮询一次 0x18/0x01，期望 8 字节应答体
         * （与已验证实现一致）。不回这个体，主机不会把
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
            ns2_body_version(&frame[8]);
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
    if (cmd == 0x01) {
        /* NFC 应答的 Status/ACK 字节按子命令取固定值（0x05 一族 00/F8、
         * 0x0C/0x15 10/78）；未命中的子命令沿用通用帧头。 */
        uint8_t nfc_status = 0;
        uint8_t nfc_ack = 0;
        if (ns2_nfc_response_ack(subcmd, &nfc_status, &nfc_ack)) {
            frame[4] = nfc_status;
            frame[5] = nfc_ack;
        }
    }
    ble_controller_notify_answer(conn_handle, resp, ANSWER_PREFIX_LEN + resp_len);
    if (cmd == 0x01) {
        /* NFC 抽块结束（0x15 EOF）后的完成事件：按 MCU 时代 read3 的形态主动
         * 补发一帧（`amiibo push <n>` 运行时选形态，0 = 关）。 */
        ns2_nfc_event_t ev;
        if (ns2_nfc_pop_event(&ev)) {
            const size_t ev_len = NS2_FRAME_HEADER_LEN + ev.body_len;
            if (ev_len <= sizeof(resp) - ANSWER_PREFIX_LEN) {
                memset(frame, 0, ev_len);
                if (ev.body_len > 0) {
                    memcpy(&frame[NS2_FRAME_HEADER_LEN], ev.body, ev.body_len);
                }
                ns2_frame_response_header(frame, cmd, transport, ev.subcmd);
                uint8_t ev_status = 0;
                uint8_t ev_ack = 0;
                if (ns2_nfc_response_ack(ev.subcmd, &ev_status, &ev_ack)) {
                    frame[4] = ev_status;
                    frame[5] = ev_ack;
                }
                ble_controller_notify_answer(conn_handle, resp, ANSWER_PREFIX_LEN + ev_len);
                ESP_LOGI(TAG, "nfc drain push 0x%02x (%uB)", ev.subcmd,
                         (unsigned)(ANSWER_PREFIX_LEN + ev_len));
            }
        }
    }
    if (!hot_path) {
        ESP_LOGI(TAG, "rsp 0x%02x/0x%02x (%uB) %s", cmd, subcmd,
                 (unsigned)(ANSWER_PREFIX_LEN + resp_len),
                 hex_prefix(resp, ANSWER_PREFIX_LEN + resp_len, rsp_hex, sizeof(rsp_hex)));
    }
}

void ns2_session_on_output(const uint8_t *data, size_t len, uint16_t conn_handle)
{
    /* Output Report 0x02：2x16B LRA 参数包。板卡无震动马达：
     * 解析为结构化震动事件经 ns2_output 分发给监听者，
     * 由监听者转发给 USB 源手柄 / 桥接 PC。 */
    (void)conn_handle;
    ns2_rumble_event_t event;
    if (!ns2_rumble_parse(data, len, &event)) {
        ESP_LOGW(TAG, "output report too short (%u)", (unsigned)len);
        return;
    }
    /* 游戏里主机的震动流接近输入上报的频率（约 66 Hz），这条回调跑在 NimBLE
     * 主机任务里：逐包日志的串口写在发送缓冲没人读时会阻塞，输入通知随之
     * 停摆，主机侧就是「操作明显变卡」。改为每秒一条汇总。 */
    static int64_t s_last_log_us;
    static uint32_t s_since_log;
    s_since_log++;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_log_us >= 1000000LL) {
        ESP_LOGI(TAG, "rumble x%u/s: L=%u R=%u (0x%02x/0x%02x)", (unsigned)s_since_log,
                 (unsigned)event.left_on, (unsigned)event.right_on, event.raw[0],
                 event.raw[16]);
        s_last_log_us = now;
        s_since_log = 0;
    }
    ns2_output_emit_rumble(&event);
}

void ns2_session_on_composite(const uint8_t *data, size_t len, uint16_t conn_handle)
{
    /* 复合输出 = 1 字节 0x00 填充 + 左右两条 16 字节 LRA 参数包 + 命令帧
     * （BlueRetro sw2 的 out_cmd 布局，Switch 2 主机的原生形态）。 */
    if (len < 33 + NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "composite too short (%u)", (unsigned)len);
        return;
    }
    /* 复合写入的震动段与 0x0012 同构（左右两条 16 字节 LRA 参数包）。只在
     * 真的在震时解析成震动事件：主机经 0x0016 下发指令时参数包段通常是
     * 静置零包（ns2-search-page.capture 全程如此），照单全收会把 0x0012 的
     * 震动流误清成停震。 */
    ns2_rumble_event_t event;
    if (ns2_rumble_parse(&data[1], 32, &event) && (event.left_on || event.right_on)) {
        ns2_output_emit_rumble(&event);
    }
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
    if (ble_stack_defer(NS2_BLE_INTENT_PAIRING)) {
        return;
    }
    /* 配对新主机（相当于按住配对键）：先断开当前主机，再发标准发现广播
     * 等新主机搜索——目标是配一台新主机，不能带着旧主机的地址广播。
     * 设备只有一台 Pro，直接进发现广播。 */
    s_ses.pairing_mode = true;
    s_pairing_until_us = esp_timer_get_time() + NS2_ADV_CONNECT_WINDOW_US;
    s_dormant_drops = 0;
    const bool was_connected = ble_controller_connected();
    if (was_connected) {
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
    }
    ESP_LOGI(TAG, "pairing request accepted (dropped link=%u, timeout=%llds)",
             (unsigned)was_connected, (long long)(NS2_ADV_CONNECT_WINDOW_US / 1000000LL));
    if (was_connected) {
        return; /* 断连事件里按配对流程起广播 */
    }
    apply_advertising();
}

bool ns2_session_pairing_mode_active(void)
{
    return s_ses.pairing_mode;
}

bool ns2_session_paired(void)
{
    return ble_creds_count(NS2_ID_PRO) > 0;
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
    /* 凭证清空后回连与唤醒都失去目标：按「从未配过」处理——静默，等用户按
     * 连接键重新配对（未配对的连接键进配对流程）。 */
    s_ses.pairing_mode = false;
    s_pairing_until_us = 0;
    ns2_adv_window_close(&s_adv_win);
    if (!ble_controller_connected()) {
        apply_advertising();
    }
    ESP_LOGI(TAG, "pairing credentials cleared");
}

/* --- 固件假升级会话：主机经 0x0018 推送升级数据的伪装接收 --- */

/** 会话状态：记录流的装配与留样在 target/ns2/ns2_upgrade.c（有主机端用例），
 *  这里只持会话级计数、应答与收尾动作。 */
#define FWUPD_ACK_BODY_MAX 16u

static struct {
    bool active;
    uint32_t writes;
    uint32_t bytes;
    size_t prev_len;
    int64_t start_us;
    int64_t last_us;
    ns2_upgrade_t up;
} s_fwupd;

/** 升级记录的应答体（帧装配完成时回给主机）：主机更新流程无公开文档，
 *  主机在数据帧后停住等应答；应答体经串口 fwack 现场替换做 A/B 对账。 */
static uint8_t s_fwupd_ack[FWUPD_ACK_BODY_MAX];
static size_t s_fwupd_ack_len;

void ns2_session_set_fw_ack_body(const uint8_t *body, size_t len)
{
    if (len > sizeof(s_fwupd_ack)) {
        len = sizeof(s_fwupd_ack);
    }
    memset(s_fwupd_ack, 0, sizeof(s_fwupd_ack));
    if (len > 0) {
        memcpy(s_fwupd_ack, body, len);
    }
    s_fwupd_ack_len = len;
}

size_t ns2_session_fw_ack_body(uint8_t *out, size_t cap)
{
    const size_t n = s_fwupd_ack_len < cap ? s_fwupd_ack_len : cap;
    if (n > 0) {
        memcpy(out, s_fwupd_ack, n);
    }
    return n;
}

/** 更新应用后上报给主机的版本（主机据此判断还要不要再推一次）：默认跟上
 *  固化值，现场用串口 fwpost 改写做 A/B。 */
static uint8_t s_fwupd_post[3] = {CONFIG_DEFAULT_FW_VERSION_MAJOR,
                                  CONFIG_DEFAULT_FW_VERSION_MINOR,
                                  CONFIG_DEFAULT_FW_VERSION_REVISION};

void ns2_session_set_fw_post_version(const uint8_t ver[3])
{
    memcpy(s_fwupd_post, ver, sizeof(s_fwupd_post));
}

void ns2_session_fw_post_version(uint8_t out[3])
{
    memcpy(out, s_fwupd_post, sizeof(s_fwupd_post));
}

/** 伪装重启分两拍：先改写上报版本并落盘（等一拍让 NVS 写完），再重启让主机
 *  看到控制器断开后带着新版本回来。
 *
 *  默认不重启：主机把「控制器重启」当成更新没生效、会自动重推整包，
 *  形成「推包 → 重启 → 再推包」的循环；重启因此改为一次性武装（串口
 *  fwapply on），触发后自动撤防。 */
static int64_t s_fwupd_apply_us;
static int64_t s_fwupd_restart_us;
static bool s_fwupd_restart_armed;

void ns2_session_set_fw_restart_armed(bool armed)
{
    s_fwupd_restart_armed = armed;
}

bool ns2_session_fw_restart_armed(void)
{
    return s_fwupd_restart_armed;
}

static void fwupd_schedule_apply(void)
{
    if (!s_fwupd_restart_armed) {
        ESP_LOGW(TAG, "fw upgrade applied by host: reboot not armed, staying up");
        return;
    }
    s_fwupd_restart_armed = false;
    s_fwupd_apply_us = esp_timer_get_time() + 1000000;
    ESP_LOGI(TAG, "fw upgrade apply armed: report version -> %u.%u.%u, reboot soon",
             s_fwupd_post[0], s_fwupd_post[1], s_fwupd_post[2]);
}

/** 按指令通道的同一套帧格式回一条应答（含 14 字节 0 前缀）。 */
static void fwupd_send_answer(uint16_t conn_handle)
{
    uint8_t resp[ANSWER_PREFIX_LEN + NS2_FRAME_HEADER_LEN + FWUPD_ACK_BODY_MAX];
    memset(resp, 0, ANSWER_PREFIX_LEN);
    uint8_t *frame = &resp[ANSWER_PREFIX_LEN];
    const uint8_t cmd = s_fwupd.up.frame[0];
    const uint8_t subcmd = s_fwupd.up.frame[3];
    ns2_frame_response_header(frame, cmd, NS2_FRAME_TRANSPORT_BLE, subcmd);
    if (s_fwupd_ack_len > 0) {
        memcpy(&frame[NS2_FRAME_HEADER_LEN], s_fwupd_ack, s_fwupd_ack_len);
    }
    const size_t len = ANSWER_PREFIX_LEN + NS2_FRAME_HEADER_LEN + s_fwupd_ack_len;
    ble_controller_notify_answer(conn_handle, resp, len);
    ESP_LOGI(TAG, "fw frame #%u complete: cmd 0x%02x/0x%02x body=%uB buffered=%uB -> rsp %uB",
             (unsigned)s_fwupd.up.frames, cmd, subcmd,
             (unsigned)ns2_upgrade_frame_body(&s_fwupd.up), (unsigned)s_fwupd.up.frame_len,
             (unsigned)s_fwupd_ack_len);
}

/** 静默视为传输结束：真实升级流程未逆向（无公开文档），这里只做统计与留样
 *  输出，不改上报版本——版本由编译期常量与串口 fwver 决定（见 ns2_frames.h）。 */
#define FWUPD_IDLE_TIMEOUT_US (10 * 1000000LL)

void ns2_session_on_fw_upgrade(const uint8_t *data, size_t len, uint16_t conn_handle)
{
    const int64_t now = esp_timer_get_time();
    if (!s_fwupd.active) {
        s_fwupd.active = true;
        s_fwupd.writes = 0;
        s_fwupd.bytes = 0;
        s_fwupd.prev_len = 0;
        s_fwupd.start_us = now;
        ns2_upgrade_reset(&s_fwupd.up);
        ESP_LOGI(TAG, "fw upgrade session started (masquerade): logging records");
    }
    const int64_t gap_us = s_fwupd.writes == 0 ? 0 : now - s_fwupd.last_us;
    s_fwupd.writes++;
    s_fwupd.bytes += (uint32_t)len;
    s_fwupd.last_us = now;
    const ns2_upgrade_event_t event = ns2_upgrade_feed(&s_fwupd.up, data, len);
    if (event == NS2_UPGRADE_MALFORMED) {
        ESP_LOGW(TAG, "fw record too short (%u)", (unsigned)len);
        s_fwupd.prev_len = len;
        return;
    }

    /* 逐条记录的日志密度：前 16 条全覆盖（看开头结构），之后每 32 条一条，
     * 长度变化时补一条——长传输不淹日志，结构切换处仍留痕。 */
    char line[160];
    const int used = snprintf(line, sizeof(line),
                              "fw rec #%u +%ums gap=%ums type=0x%02x idx=%u %uB total=%u",
                              (unsigned)s_fwupd.writes,
                              (unsigned)((now - s_fwupd.start_us) / 1000),
                              (unsigned)(gap_us / 1000), data[0], data[1], (unsigned)len,
                              (unsigned)s_fwupd.bytes);
    const bool verbose = s_fwupd.writes <= 16 || (s_fwupd.writes % 32) == 0 ||
                         (s_fwupd.writes > 1 && len != s_fwupd.prev_len);
    if (verbose && used > 0 && (size_t)used < sizeof(line)) {
        char preview[3 * 16 + 4];
        snprintf(&line[used], sizeof(line) - (size_t)used, " %s",
                 hex_prefix(data, len, preview, sizeof(preview)));
    }
    ESP_LOGI(TAG, "%s", line);
    s_fwupd.prev_len = len;

    if (event == NS2_UPGRADE_FRAME) {
        fwupd_send_answer(conn_handle);
    }
}

static void fwupd_finish(void)
{
    const uint32_t bytes = s_fwupd.bytes;
    const int64_t span_us = s_fwupd.last_us - s_fwupd.start_us;
    ESP_LOGI(TAG, "fw upgrade idle -> done: %u frames, %u records, %u bytes, span %us, rec %u..%uB",
             (unsigned)s_fwupd.up.frames, (unsigned)s_fwupd.up.records, (unsigned)bytes,
             (unsigned)(span_us / 1000000),
             (unsigned)(s_fwupd.up.min_record == SIZE_MAX ? 0 : s_fwupd.up.min_record),
             (unsigned)s_fwupd.up.max_record);
    if (s_fwupd.up.truncated) {
        ESP_LOGW(TAG, "fw upgrade: frame over %uB buffer", (unsigned)NS2_UPGRADE_FRAME_CAP);
    }
    log_hex_block("fw frame", s_fwupd.up.sample, s_fwupd.up.sample_len,
                  sizeof(s_fwupd.up.sample));
    log_hex_block("fw tail", s_fwupd.up.tail, s_fwupd.up.tail_len, sizeof(s_fwupd.up.tail));
    s_fwupd.active = false;
}

/** 配对流程是否已经完成：当前形态的每个身份都「有凭证且已进入注册会话」。
 *  判据不能只看凭证——已配对设备本来就带着凭证，一按配对键就会被判成完成、
 *  一秒内退回「已配对」（配对键看起来毫无作用）；也不能只看连接。 */
static bool pairing_flow_done(void)
{
    ns2_identity_t ids[2];
    const size_t n = mode_identities(ids);
    for (size_t i = 0; i < n; i++) {
        if (ble_creds_count(ids[i]) == 0) {
            return false;
        }
        const session_slot_t *slot = session_by_identity(ids[i]);
        if (slot == NULL || slot->state != SESSION_NORMAL) {
            return false;
        }
    }
    return true;
}

void ns2_session_tick(void)
{
    if (s_fwupd.active && esp_timer_get_time() - s_fwupd.last_us > FWUPD_IDLE_TIMEOUT_US) {
        fwupd_finish();
    }

    /* 假升级的收尾：改写上报版本并落盘，一拍后重启（见 fwupd_schedule_apply）。 */
    if (s_fwupd_apply_us != 0 && esp_timer_get_time() >= s_fwupd_apply_us) {
        s_fwupd_apply_us = 0;
        app_config_set_fw_version(s_fwupd_post);
        app_config_flush();
        ESP_LOGI(TAG, "fw upgrade applied: report version -> %u.%u.%u, restarting soon",
                 s_fwupd_post[0], s_fwupd_post[1], s_fwupd_post[2]);
        s_fwupd_restart_us = esp_timer_get_time() + 1500000;
    }
    if (s_fwupd_restart_us != 0 && esp_timer_get_time() >= s_fwupd_restart_us) {
        s_fwupd_restart_us = 0;
        ESP_LOGW(TAG, "fw upgrade restart (masquerade)");
        esp_restart();
    }

    const int64_t now = esp_timer_get_time();

    /* 静默计时：静默持续够久（NS2_BLE_IDLE_GRACE_US）才允许关栈省电。 */
    s_idle_since_us = ns2_session_stack_idle() ? (s_idle_since_us != 0 ? s_idle_since_us : now) : 0;

    /* 广播窗口到期（连接窗口 / 唤醒窗口）：设备回到静默——不会一直发
     * 信号，想再连一次就再按一次连接键或 HOME。 */
    if (s_adv_win.until_us != 0 && !ns2_adv_window_active(&s_adv_win, now)) {
        ns2_adv_window_close(&s_adv_win);
        ESP_LOGI(TAG, "advertising window expired: silent");
        if (!ble_controller_connected()) {
            apply_advertising();
        }
    } else if (ns2_adv_window_active(&s_adv_win, now) && !ble_controller_connected()) {
        /* 窗口内分时形态检测：如前 3 秒唤醒突发结束，切回 0x00 回连广播。 */
        const uint8_t *mac = NULL;
        const ns2_adv_mode_t cur_mode = adv_mode_for(NS2_ID_PRO, &mac);
        if (cur_mode != s_last_applied_adv_mode) {
            apply_advertising();
        }
    }

    /* 配对流程超时收尾：30 秒未配对成功，自动退出配对流程并停发广播。 */
    if (s_ses.pairing_mode && s_pairing_until_us != 0 && now >= s_pairing_until_us) {
        s_ses.pairing_mode = false;
        s_pairing_until_us = 0;
        ESP_LOGI(TAG, "pairing flow timed out: silent");
        if (!ble_controller_connected()) {
            apply_advertising();
        }
    }

    /* 配对流程收尾：主机真的配好并连上（当前形态每个身份都凭证在手、会话
     * 注册完成）才自动退出——配完就处于已连接状态，不需要用户再按；
     * 没有主机来配就一直挂着发现广播，等用户按停止或主机来配。 */
    if (s_ses.pairing_mode && pairing_flow_done()) {
        s_ses.pairing_mode = false;
        s_pairing_until_us = 0;
        ESP_LOGI(TAG, "pairing flow finished (host registered)");
        if (!ble_controller_connected()) {
            apply_advertising();
        }
    }

    /* 休眠看门狗：已订阅输入却始终没启用特性（ns2_adv_dormant_link）持续
     * NS2_DORMANT_TICKS 秒的连接，主机永远不会采用它的输入——断开并开一次
     * 断连回连信号窗口（只发回连形态），逼主机按会启用特性的回连路径重连。
     * 主机此刻就在线（它是醒着的），不带唤醒突发，免得把可能刚睡下的主机
     * 叫醒。手动配对模式下不干预（此时由用户主导流程）。 */
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
            ESP_LOGW(TAG, "dormant link (conn=%u, features not enabled) -> drop + reconnect "
                     "(attempt %u/%u)", ses->conn_handle,
                     (unsigned)s_dormant_drops, (unsigned)NS2_DORMANT_MAX_DROPS);
            ns2_adv_window_open(&s_adv_win, &NS2_ADV_SIGNAL_RECONNECT, esp_timer_get_time());
            ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
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
             *  队列（休眠连接上曾见近半数通知因拥塞失败）。
             *  但 READ 缓存要跟着刷新：主机在握把页的行只发了 0x0c/0x02、
             *  没订阅输入通道，靠 READ 轮询取输入值——
             *  缓存不刷新，主机读到的永远是全零。 */
            if (!slot->features_enabled) {
                ble_controller_store_input(conn, report_id, body);
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

/** 上报版本改动后重建出厂块：0x10 查询直接读配置即刻生效，两个出厂块
 * （0x7E40 / 0x13000）的版本字段在工厂数据里，重建一次才对得上。 */
void ns2_session_refresh_fw_version(void)
{
    factory_init();
}

static bool apply_colors(uint32_t body_rgb, uint32_t button_rgb, uint32_t accent_rgb,
                         uint32_t grip_rgb);

/** 控制面下发四段配色：重建出厂块，并打开连接窗口让主机把这只「新手柄」
 *  接回来（配色是主机连上时读的，换配色必须重新连一次；见 apply_colors）。 */
void ns2_session_set_colors(uint32_t body_rgb, uint32_t button_rgb, uint32_t accent_rgb,
                            uint32_t grip_rgb)
{
    apply_colors(body_rgb, button_rgb, accent_rgb, grip_rgb);
}


static bool apply_colors(uint32_t body_rgb, uint32_t button_rgb, uint32_t accent_rgb,
                         uint32_t grip_rgb)
{
    const bool changed = s_ses.body_color != body_rgb ||
                         s_ses.button_color != button_rgb ||
                         s_ses.accent_color != accent_rgb ||
                         s_ses.grip_color != grip_rgb;
    if (!changed) {
        return false;
    }
    s_ses.body_color = body_rgb;
    s_ses.button_color = button_rgb;
    s_ses.accent_color = accent_rgb;
    s_ses.grip_color = grip_rgb;
    /* 配色变化要重建出厂块，重新武装在线行便于对照日志。 */
    s_ses.pair_online_logged = false;
    s_dormant_drops = 0;
    if (s_ses.synced) {
        /* 等价于手柄断电再上电：重算出厂块，并组装信号搜索信号开窗——主机
         * 连回来读到的就是新颜色（用户不必再按一次连接键）。
         * 未配对时没有主机可回连，保持静默：设备不被请求连接就不发信号。 */
        s_ses.pairing_mode = false;
        s_pairing_until_us = 0;
        factory_init();
        if (ns2_session_paired()) {
            ns2_adv_window_open(&s_adv_win, &NS2_ADV_SIGNAL_SEARCH, esp_timer_get_time());
        } else {
            ns2_adv_window_close(&s_adv_win);
        }
    }
    ESP_LOGI(TAG, "controller colors -> body=%06lx btn=%06lx accent=%06lx grip=%06lx "
             "(paired=%u pairing=%u)",
             (unsigned long)body_rgb, (unsigned long)button_rgb,
             (unsigned long)accent_rgb, (unsigned long)grip_rgb,
             (unsigned)ns2_session_paired(), (unsigned)s_ses.pairing_mode);
    if (ble_controller_connected()) {
        ble_controller_disconnect(BLE_CTL_DISCONNECT_USER_TERM);
        return true; /* 断连事件里按新配色同步广播（窗口已开即回连形态） */
    }
    if (s_ses.synced) {
        apply_advertising();
    }
    return true;
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
    if (out == NULL || identity != NS2_ID_PRO) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->identity = identity;
    out->creds = (uint8_t)ble_creds_count((ns2_identity_t)identity);
    out->advertising = ble_controller_adv_running(identity);
    /* 该身份当前（或恢复时）会用的广播形态，供串口对账「设备在发哪一种」。 */
    const uint8_t *adv_mac = NULL;
    out->adv_mode = (uint8_t)adv_mode_for((ns2_identity_t)identity, &adv_mac);
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
    /* 专用通道与通用通道分开看：传 0x09 只表示「非 0x05 的那一路」。 */
    out->notify_priv = ble_controller_input_notify_ready(slot->conn_handle, NS2_REPORT_ID_09);
    ble_controller_input_priv_handle(slot->conn_handle, &out->notify_priv_handle);
    out->features_enabled = slot->features_enabled;
    out->reports = slot->reports;
    ble_controller_conn_itvl(slot->conn_handle, &out->conn_itvl);
    out->state = slot->state == SESSION_NORMAL ? NS2_LINK_NORMAL : NS2_LINK_WAIT_PAIR;
    return true;
}

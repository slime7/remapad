#include "ble_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_store.h"
#include "psa/crypto.h"

#include "ble_controller.h"
#include "ble_creds.h"
#include "ns2_frames.h"
#include "ns2_output.h"
#include "ns2_report.h"

static const char *TAG = "remapad_blses";

/** 发现广播与出厂块对外的 PID：Pro Controller 2（0x2069）默认，JoyCon 组合
 *  模式切换为 Joy-Con 2 (L)（0x2067）；由手柄身份配置驱动。 */
#define NS2_PID_PRO 0x2069
#define NS2_PID_JOYCON_L 0x2067

/** 手柄身份：类型 + 出厂块配色（0 = 沿用默认占位）。控制面经
 *  ns2_session_set_identity 下发（持久化在 app_config）。 */
static struct {
    bool joycon;
    uint32_t body_color;
    uint32_t button_color;
    uint32_t grip_color;
} s_identity;

/** 出厂数据区仿真基址（controller.md §7.2）。 */
#define FACTORY_BASE 0x013000u
#define FACTORY_SIZE 2048u

/** 会话状态：休眠/唤醒广播顺延到后续里程碑（见 docs/ROADMAP.md M3）。 */
typedef enum {
    SESSION_ADV_DISCOVERY = 0,
    SESSION_ADV_RECONNECT,
    SESSION_CONNECTED_WAIT_PAIR,
    SESSION_NORMAL,
} session_state_t;

static struct {
    session_state_t state;
    uint8_t own_mac[6];
    uint8_t report_format;
    uint8_t feature_mask;
    uint8_t player_leds;
    bool pairing_mode;
} s_ses;

/** 0x15 配对会话中间态：主机 MAC 与派生 LTK（线格式字节序）。 */
static struct {
    uint8_t host_mac[6];
    bool mac_ready;
    uint8_t ltk[16];
    bool ltk_ready;
} s_pair;

static uint8_t s_factory[FACTORY_SIZE];

/** host 同步完成标志（身份变更时判断是否重建出厂块与广播）。 */
static bool s_synced;

/** 广播恢复前置声明（身份变更时复用）。 */
static void resume_advertising(void);

/** 0x13000 出厂数据块（实机抓包布局）：`01 00` + 序列号@2 + `00 00`
 * + VID/PID@18 + 版本@22 + 机身配色@25，尾部 0xFF。 */
static uint8_t s_mem_factory[64];

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

/** 指令应答的通知载荷 = 14 字节 0x00 前缀 + 8B 帧头 + 应答体（实机抓包）。 */
#define ANSWER_PREFIX_LEN 14

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

/** 当前身份的广播 / 出厂 PID。 */
static uint16_t identity_pid(void)
{
    return s_identity.joycon ? NS2_PID_JOYCON_L : NS2_PID_PRO;
}

/** 出厂数据区：序列号、VID/PID、机身配色与摇杆校准（controller.md §7.2）。
 * 校准取中位 2048、行程 ±2047/2048，与编码器 0-4095 直发语义保持 1:1。
 * 序列号与配色按手柄身份配置生成（Pro: HEJ 前缀；JoyCon L: HBW 前缀）。 */
static void factory_init(void)
{
    memset(s_factory, 0xFF, sizeof(s_factory));
    memcpy(&s_factory[0x0002], "REMAPAD-S3-0001", 15);
    s_factory[0x0011] = 0x00;
    s_factory[0x0012] = 0x7E;
    s_factory[0x0013] = 0x05;
    s_factory[0x0014] = (uint8_t)(identity_pid() & 0xFF);
    s_factory[0x0015] = (uint8_t)(identity_pid() >> 8);
    /* 机身/按键/高光/握把配色：未配置时沿用深灰占位。 */
    const uint32_t body = s_identity.body_color != 0 ? s_identity.body_color : 0x232323;
    const uint32_t button = s_identity.button_color != 0 ? s_identity.button_color : 0x3c3c3c;
    const uint32_t grip = s_identity.grip_color != 0 ? s_identity.grip_color : 0x2e2e2e;
    s_factory[0x0019] = (uint8_t)(body >> 16);
    s_factory[0x001A] = (uint8_t)(body >> 8);
    s_factory[0x001B] = (uint8_t)(body);
    s_factory[0x001C] = (uint8_t)(button >> 16);
    s_factory[0x001D] = (uint8_t)(button >> 8);
    s_factory[0x001E] = (uint8_t)(button);
    s_factory[0x001F] = (uint8_t)(button >> 16);
    s_factory[0x0020] = (uint8_t)(button >> 8);
    s_factory[0x0021] = (uint8_t)(button);
    s_factory[0x0022] = (uint8_t)(grip >> 16);
    s_factory[0x0023] = (uint8_t)(grip >> 8);
    s_factory[0x0024] = (uint8_t)(grip);
    static const uint8_t stick_cal[9] = {0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80};
    memcpy(&s_factory[0x00A8], stick_cal, sizeof(stick_cal));
    memcpy(&s_factory[0x00E8], stick_cal, sizeof(stick_cal));

    /* 0x13000 出厂数据块（实机抓包布局）：`01 00` + 序列号@2 + `00 00`
     * + VID/PID@18 + 版本@22 + 机身配色@25，尾部未用区 0xFF。 */
    memset(s_mem_factory, 0xFF, sizeof(s_mem_factory));
    s_mem_factory[0] = 0x01;
    s_mem_factory[1] = 0x00;
    /* 序列号格式对齐真机（3 字母前缀 + 11 位数字），主机可能校验其形态：
     * Pro 用 HEJ 前缀；JoyCon 组合按左侧手柄用 HBW 前缀（右为 HCW，仅
     * UI 展示，单连接以 L 身份广播）。 */
    memcpy(&s_mem_factory[2], s_identity.joycon ? "HBW1006700000" : "HEJ71001123456", 14);
    s_mem_factory[18] = 0x7E;
    s_mem_factory[19] = 0x05;
    s_mem_factory[20] = (uint8_t)(identity_pid() & 0xFF);
    s_mem_factory[21] = (uint8_t)(identity_pid() >> 8);
    s_mem_factory[22] = 0x01;
    s_mem_factory[23] = 0x06;
    s_mem_factory[24] = 0x01;
    const uint8_t body_colors[12] = {
        (uint8_t)(body >> 16), (uint8_t)(body >> 8), (uint8_t)(body),
        (uint8_t)(button >> 16), (uint8_t)(button >> 8), (uint8_t)(button),
        (uint8_t)(button >> 16), (uint8_t)(button >> 8), (uint8_t)(button),
        (uint8_t)(grip >> 16), (uint8_t)(grip >> 8), (uint8_t)(grip),
    };
    memcpy(&s_mem_factory[25], body_colors, sizeof(body_colors));
}

/** 控制面下发手柄身份（类型 + 配色）：更新出厂块；host 已同步时立即
 *  重建，下次广播/握手生效。 */
void ns2_session_set_identity(bool joycon, uint32_t body_rgb,
                              uint32_t button_rgb, uint32_t grip_rgb)
{
    const bool changed = s_identity.joycon != joycon ||
                         s_identity.body_color != body_rgb ||
                         s_identity.button_color != button_rgb ||
                         s_identity.grip_color != grip_rgb;
    if (!changed) {
        return;
    }
    s_identity.joycon = joycon;
    s_identity.body_color = body_rgb;
    s_identity.button_color = button_rgb;
    s_identity.grip_color = grip_rgb;
    if (s_synced) {
        factory_init();
        /* 广播 PID 随身份变化，重建当前广播载荷。 */
        if (!ble_controller_connected()) {
            resume_advertising();
        }
    }
    ESP_LOGI(TAG, "controller identity -> %s (body=%06lx btn=%06lx grip=%06lx)",
             joycon ? "joycon-l" : "pro", (unsigned long)body_rgb,
             (unsigned long)button_rgb, (unsigned long)grip_rgb);
}

/** 31 字节手柄广播载荷（controller.md §2.1）：Flags 3B + 厂商数据 28B。
 * 偏移：5-6 Company ID、7-9 协议头、10-11 VID、12-13 PID、16 状态位、
 * 17-22 目标主机 MAC 反序、23 尾部标志。有凭证时构造回连广播。 */
static void build_adv_payload(uint8_t out[31], bool reconnect)
{
    const uint16_t pid = identity_pid();
    static const uint8_t tpl[31] = {
        0x02, 0x01, 0x06,
        0x1B, 0xFF,
        0x53, 0x05, 0x01, 0x00, 0x03,
        0x7E, 0x05,
        0x00, 0x00,
        0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(out, tpl, 31);
    out[12] = (uint8_t)(pid & 0xFF);
    out[13] = (uint8_t)(pid >> 8);
    if (reconnect && ble_creds_count() > 0) {
        const ns2_cred_record_t *rec = ble_creds_get(0);
        memcpy(&out[17], rec->mac, 6);
        /* 已配对回连/唤醒形态：状态字节 0x81（对齐已验证实现，
         * 主机据此按回连流程初始化）。 */
        out[16] = 0x81;
        s_ses.state = SESSION_ADV_RECONNECT;
    } else {
        s_ses.state = SESSION_ADV_DISCOVERY;
    }
}

/** 按凭证状态恢复广播：已配对发回连广播等待主机回连；未配对发发现广播
 * 待主机从 Grip 界面搜索配对，开机即进入可被发现状态。 */
static void resume_advertising(void)
{
    const size_t creds = ble_creds_count();
    uint8_t adv[31];
    build_adv_payload(adv, creds > 0);
    ble_controller_advertise(adv);
    if (creds > 0) {
        ESP_LOGI(TAG, "resume: reconnect advertising (%u creds)", (unsigned)creds);
    } else {
        ESP_LOGI(TAG, "resume: discovery advertising (no creds)");
    }
}

void ns2_session_on_sync(const uint8_t own_mac[6])
{
    factory_init();
    s_synced = true;
    memcpy(s_ses.own_mac, own_mac, 6);
    ESP_LOGI(TAG, "host synced, own MAC %02x:%02x:%02x:%02x:%02x:%02x",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
    resume_advertising();
}

/** 连接空闲超时（微秒）：主机连上后会立刻跑初始化序列（毫秒级到达），
 * 手机/PC 的自动回连则连上后无任何协议活动；超时未活动即断开，
 * 兼顾主机初始化与回连骚扰清理。主机连接地址是随机地址，
 * 无法按 OUI 识别，不能再用地址白名单。 */
#define HOST_IDLE_TIMEOUT_US (3 * 1000000LL)

static int64_t s_last_activity_us;

void ns2_session_touch(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/** 当前连接是否为应被清理的空闲主机：已连接、握手未完成且超时无活动。 */
bool ns2_session_host_idle_expired(void)
{
    return ble_controller_connected() && s_ses.state == SESSION_CONNECTED_WAIT_PAIR &&
           esp_timer_get_time() - s_last_activity_us > HOST_IDLE_TIMEOUT_US;
}

/** 将派生 LTK 注入 NimBLE bonding store（随机数与 EDIV 全 0，BLE 链路
 * 加密用的 LTK 为线序 A1 XOR B1）；主机配对完成或回连后调用。 */
static void inject_ltk_to_ble_store(const uint8_t host_mac[6], const uint8_t ltk[16])
{
    struct ble_store_value_sec sec;
    memset(&sec, 0, sizeof(sec));
    sec.bond_count = 1;
    sec.key_size = 16;
    sec.ltk_present = 1;
    reverse_bytes(ltk, sec.ltk, 16);
    sec.peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(sec.peer_addr.val, host_mac, 6);
    sec.rand_num = 0;
    sec.ediv = 0;
    sec.authenticated = 1;
    sec.sc = 1;
    ble_store_write_our_sec(&sec);
    ble_store_write_peer_sec(&sec);
}

void ns2_session_on_connect(uint16_t conn_handle)
{
    uint8_t peer[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const ns2_cred_record_t *matched = NULL;
    const bool have_peer = ble_controller_peer_mac(conn_handle, peer);
    if (have_peer) {
        for (size_t i = 0; i < ble_creds_count(); i++) {
            if (memcmp(ble_creds_get(i)->mac, peer, 6) == 0) {
                matched = ble_creds_get(i);
                break;
            }
        }
    }
    ESP_LOGI(TAG, "connected (conn=%u, peer %02x:%02x:%02x:%02x:%02x:%02x, %s)",
             conn_handle, peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
             matched ? "paired host" : "unpaired host");
    s_ses.state = matched ? SESSION_NORMAL : SESSION_CONNECTED_WAIT_PAIR;
    s_ses.pairing_mode = false;
    ns2_session_touch();
    if (matched) {
        /* 回连：把 NVS 里的 LTK 重新注入本周期 NimBLE RAM store。 */
        inject_ltk_to_ble_store(matched->mac, matched->ltk);
    }
    ESP_LOGI(TAG, "waiting host init sequence");
}

void ns2_session_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected, resume advertising");
    resume_advertising();
}

/** SPI 模拟内存映射块。0x13040 与 0x13100 为实机固定内容；0x13060 与
 * 用户自定义校准区（0x1FC000 运动 / 0x1FC040 主摇杆 / 0x1FC060 副摇杆）
 * 未经写入即未初始化，长度 0、读出为全 0xFF。 */
static const struct {
    uint32_t start;
    size_t len;
    const uint8_t *data;
} s_mem_map[] = {
    {0x013000u, sizeof(s_mem_factory), s_mem_factory},
    {0x013040u, 16,
     (const uint8_t[]){0x3B, 0xE0, 0xD3, 0x41, 0xC6, 0x60, 0x6A, 0xBC,
                       0x4D, 0xD7, 0xA2, 0xBB, 0x71, 0x1E, 0xDD, 0x37}},
    {0x013060u, 0, NULL},
    {0x013080u, sizeof(s_mem_cal80), s_mem_cal80},
    {0x0130c0u, sizeof(s_mem_calc0), s_mem_calc0},
    {0x013100u, 24,
     (const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0xA6, 0xF2, 0x62, 0xBD,
                       0xA8, 0x00, 0x08, 0x3D, 0x2F, 0xED, 0x20, 0x41}},
    {0x01fc000u, 0, NULL},
    {0x01fc040u, 0, NULL},
    {0x01fc060u, 0, NULL},
};

/** Command 0x02 SPI 读取：应答体 = 回显请求 [8:16] 的 8B magic（[1] 清零）
 * + 命中数据。地址为请求 [12:15] 的 3 字节小端，按 s_mem_map 区间重叠取数，
 * 命中范围内未覆盖的部分补 0xFF；无任何命中返回空应答体。 */
static size_t handle_flash_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp, size_t cap)
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
    const uint32_t addr = ((uint32_t)req[14] << 16) | ((uint32_t)req[13] << 8) | req[12];
    const uint32_t addr_end = addr + rlen;
    memset(&resp[16], 0xFF, rlen);
    bool hit = false;
    for (size_t i = 0; i < sizeof(s_mem_map) / sizeof(s_mem_map[0]); i++) {
        const uint32_t block_start = s_mem_map[i].start;
        const uint32_t block_end = block_start + s_mem_map[i].len;
        if (block_end < addr || block_start > addr_end) {
            continue;
        }
        hit = true;
        if (s_mem_map[i].len == 0) {
            continue;
        }
        const uint32_t overlap_start = addr > block_start ? addr : block_start;
        const uint32_t overlap_end = addr_end < block_end ? addr_end : block_end;
        memcpy(&resp[16] + (overlap_start - addr),
               &s_mem_map[i].data[overlap_start - block_start],
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
static size_t handle_init_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp)
{
    switch (subcmd) {
    case 0x01:
        ESP_LOGI(TAG, "wake advertising request=%u (deferred)", req[8]);
        return NS2_FRAME_HEADER_LEN;
    case 0x0A:
        if (len >= 9 && (req[8] == NS2_REPORT_ID_05 || req[8] == NS2_REPORT_ID_09)) {
            s_ses.report_format = req[8];
            ESP_LOGI(TAG, "input report format -> 0x%02x", req[8]);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x07:
        /* 直接注入配对信息：6B 主机 MAC（反序）+ 16B LTK（反序）。
         * MAC 按线格式原样存储；LTK 反转回派生形态（0x15/0x04 路径的 A1^B1）。 */
        if (len >= NS2_FRAME_HEADER_LEN + 22) {
            uint8_t ltk[16];
            reverse_bytes(&req[14], ltk, 16);
            ble_creds_save(&req[8], ltk);
            s_ses.state = SESSION_NORMAL;
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x08:
        ble_creds_clear();
        s_ses.state = SESSION_CONNECTED_WAIT_PAIR;
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
static size_t handle_led_cmd(const uint8_t *req, size_t len, uint8_t subcmd)
{
    switch (subcmd) {
    case 0x01:
        s_ses.player_leds = 0x01;
        break;
    case 0x02:
        s_ses.player_leds = 0x02;
        break;
    case 0x03:
        s_ses.player_leds = 0x04;
        break;
    case 0x04:
        s_ses.player_leds = 0x08;
        break;
    case 0x05:
        s_ses.player_leds = 0x0F;
        break;
    case 0x06:
        s_ses.player_leds = 0x00;
        break;
    case 0x07:
        s_ses.player_leds = len >= 9 ? (uint8_t)(req[8] & 0x0F) : 0;
        break;
    default:
        ESP_LOGI(TAG, "led subcmd 0x%02x (blink)", subcmd);
        return NS2_FRAME_HEADER_LEN;
    }
    ESP_LOGI(TAG, "player LED mask -> 0x%x", s_ses.player_leds);
    ns2_output_emit_player_led(s_ses.player_leds);
    return NS2_FRAME_HEADER_LEN;
}

/** Command 0x0C 特性掩码：body 首字节生效（bit5 触觉影响 0x09 状态标志）。 */
static size_t handle_feature_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp)
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
        s_ses.feature_mask = mask;
        break;
    case 0x04:
        s_ses.feature_mask |= mask;
        break;
    case 0x05:
        s_ses.feature_mask = (uint8_t)(s_ses.feature_mask & ~mask);
        break;
    case 0x06:
        ESP_LOGI(TAG, "feature sampling config 0x%02x (ignored)", mask);
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "feature mask -> 0x%02x (subcmd 0x%02x)", s_ses.feature_mask, subcmd);
    memset(&resp[8], 0, 4);
    return NS2_FRAME_HEADER_LEN + 4;
}

/** Command 0x15 私有配对四步（controller.md §3）：MAC 交换 -> 公钥交换 ->
 * AES-128-ECB 挑战 -> 确认保存；全程不涉及标准 SMP。请求体以 0x00 前缀、
 * 应答体以 0x01 前缀（实机抓包，ndeadly/switch2_controller_research）。 */
static size_t handle_pairing_cmd(const uint8_t *req, size_t len, uint8_t subcmd,
                                 uint8_t *resp, size_t cap)
{
    switch (subcmd) {
    case 0x01:
        /* 步骤 1：请求体 `00 [地址数] [地址数×6B 主机地址反序]`（主机发两个地址，
         * 绑定首个）；应答体 `01 04 01 [自身地址反序]`。 */
        if (len < NS2_FRAME_HEADER_LEN + 8 || cap < NS2_FRAME_HEADER_LEN + 9) {
            return 0;
        }
        if (req[9] >= 1) {
            memcpy(s_pair.host_mac, &req[10], 6);
            s_pair.mac_ready = true;
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
            s_pair.ltk[i] = (uint8_t)(req[9 + (15 - i)] ^ ns2_pair_pubkey_b1[15 - i]);
        }
        s_pair.ltk_ready = true;
        resp[8] = 0x01;
        memcpy(&resp[9], ns2_pair_pubkey_b1, 16);
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x02:
        /* 步骤 3：请求体 `00 [16B 挑战码 A2 反序]`；
         * B2 = AES128_ECB(Key=存储 LTK, Data=reverse(A2))，应答体
         * `01 [16B B2 原始输出]`（不再反转，对齐已验证实现）。 */
        if (!s_pair.ltk_ready || len < NS2_FRAME_HEADER_LEN + 17 ||
            cap < NS2_FRAME_HEADER_LEN + 17) {
            return 0;
        }
        {
            uint8_t a2[16];
            uint8_t b2[16];
            reverse_bytes(&req[9], a2, 16);
            if (!aes_ecb_block(s_pair.ltk, a2, b2)) {
                ESP_LOGE(TAG, "psa aes failed");
                return 0;
            }
            resp[8] = 0x01;
            memcpy(&resp[9], b2, 16);
        }
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x03:
        /* 步骤 4：确认并持久化主机 MAC + LTK，同时注入 NimBLE bonding
         * store（rand/ediv 全 0），主机后续的标准加密请求即可用该 LTK。 */
        if (s_pair.mac_ready && s_pair.ltk_ready) {
            ble_creds_save(s_pair.host_mac, s_pair.ltk);
            inject_ltk_to_ble_store(s_pair.host_mac, s_pair.ltk);
            s_pair.mac_ready = false;
            s_pair.ltk_ready = false;
            s_ses.state = SESSION_NORMAL;
        }
        resp[8] = 0x01;
        return NS2_FRAME_HEADER_LEN + 1;
    default:
        ESP_LOGW(TAG, "pairing subcmd 0x%02x unsupported", subcmd);
        return NS2_FRAME_HEADER_LEN;
    }
}

void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport)
{
    if (len < NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "command too short (%u)", (unsigned)len);
        return;
    }
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
        resp_len = handle_flash_cmd(data, len, subcmd, frame, sizeof(resp) - ANSWER_PREFIX_LEN);
        break;
    case 0x03:
        resp_len = handle_init_cmd(data, len, subcmd, frame);
        break;
    case 0x09:
        resp_len = handle_led_cmd(data, len, subcmd);
        break;
    case 0x0A: {
        const uint8_t sample = subcmd == 0x02 && len >= 9 ? data[8] : subcmd;
        ESP_LOGI(TAG, "haptic sample 0x%02x", sample);
        ns2_output_emit_haptic_sample(sample);
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    }
    case 0x0C:
        resp_len = handle_feature_cmd(data, len, subcmd, frame);
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
    case NS2_CMD_VERSION:
        if (subcmd == 0x01) {
            ns2_body_version(&frame[8]);
            resp_len = NS2_FRAME_HEADER_LEN + NS2_VERSION_BODY_LEN;
        } else {
            resp_len = NS2_FRAME_HEADER_LEN;
        }
        break;
    case NS2_CMD_PAIRING:
        resp_len = handle_pairing_cmd(data, len, subcmd, frame, sizeof(resp) - ANSWER_PREFIX_LEN);
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
    ble_controller_notify_answer(resp, ANSWER_PREFIX_LEN + resp_len);
}

void ns2_session_on_output(const uint8_t *data, size_t len)
{
    /* Output Report 0x02：BLE 形态首字节 0x00，随后 2x16B LRA 参数包（§5.4）。
     * 板卡无震动马达：解析为结构化震动事件经 ns2_output 分发给监听者
     * （当前记录日志，M5 起转发给 USB 源手柄 / 桥接 PC）。 */
    if (len < 1 + 32) {
        ESP_LOGW(TAG, "output report too short (%u)", (unsigned)len);
        return;
    }
    ns2_rumble_event_t event;
    memcpy(event.raw, &data[1], sizeof(event.raw));
    /* LRA 状态字 bit6 = 启用标志（controller.md §5.4）。 */
    event.left_on = (event.raw[0] & 0x40) != 0;
    event.right_on = (event.raw[16] & 0x40) != 0;
    ESP_LOGI(TAG, "rumble: L=%u R=%u (0x%02x/0x%02x)",
             (unsigned)event.left_on, (unsigned)event.right_on,
             event.raw[0], event.raw[16]);
    ns2_output_emit_rumble(&event);
}

void ns2_session_on_composite(const uint8_t *data, size_t len)
{
    /* 实机抓包：复合输出 = 33 字节 0x00 填充 + 命令帧（首字节为震动形态
     * 的 Switch 1 布局未被 Switch 2 主机使用）。 */
    if (len < 33 + NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "composite too short (%u)", (unsigned)len);
        return;
    }
    ns2_session_on_output(data, 32);
    ns2_session_on_command(&data[33], len - 33, NS2_FRAME_TRANSPORT_BLE);
}

uint8_t ns2_session_report_format(void)
{
    return s_ses.report_format;
}

bool ns2_session_rumble_enabled(void)
{
    return (s_ses.feature_mask & 0x20) != 0;
}

void ns2_session_start_pairing_mode(void)
{
    s_ses.pairing_mode = true;
    uint8_t adv[31];
    build_adv_payload(adv, false);
    ble_controller_advertise(adv);
    ESP_LOGI(TAG, "pairing mode: discovery advertising");
}

void ns2_session_stop_pairing_mode(void)
{
    s_ses.pairing_mode = false;
    resume_advertising();
    ESP_LOGI(TAG, "pairing mode stopped");
}

bool ns2_session_pairing_mode_active(void)
{
    return s_ses.pairing_mode;
}

bool ns2_session_paired(void)
{
    return ble_creds_count() > 0;
}

bool ns2_session_host_registered(void)
{
    return ble_controller_connected() && s_ses.state == SESSION_NORMAL;
}

bool ns2_session_waiting_pair(void)
{
    return ble_controller_connected() && s_ses.state == SESSION_CONNECTED_WAIT_PAIR;
}

void ns2_session_unpair(void)
{
    ble_creds_clear();
    /* 凭证清空后回连广播失效，恢复为未配对静默。 */
    resume_advertising();
    ESP_LOGI(TAG, "pairing credentials cleared");
}

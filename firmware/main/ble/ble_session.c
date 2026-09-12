#include "ble_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

#include "ble_controller.h"
#include "ble_creds.h"
#include "ns2_frames.h"
#include "ns2_report.h"

static const char *TAG = "remapad_blses";

/** 手柄身份：Pro Controller 2（controller.md §1）。 */
#define NS2_PID_PRO_CONTROLLER_2 0x2069

/** 会话状态：休眠/唤醒广播顺延到后续里程碑（见 docs/ROADMAP.md M3）。 */
typedef enum {
    SESSION_ADV_DISCOVERY = 0,
    SESSION_ADV_RECONNECT,
    SESSION_CONNECTED_WAIT_PAIR,
    SESSION_NORMAL,
} session_state_t;

/** 出厂数据区仿真基址（controller.md §7.2）。 */
#define FACTORY_BASE 0x013000u
#define FACTORY_SIZE 2048u

/** 配对信息区仿真基址（controller.md §7.4），结构对齐 0x1FA000。 */
#define PAIRING_BASE 0x1FA000u
#define PAIRING_IMAGE_SIZE 256u

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

/** 主机经 0x02/0x04 实际读取的出厂块（实机抓包基址 0x7E40，64B）：
 * 序列号、VID/PID、机身配色。 */
static uint8_t s_factory_block[64];

/** 指令应答的通知载荷 = 14 字节 0x00 前缀 + 8B 帧头 + 应答体（实机抓包）。 */
#define ANSWER_PREFIX_LEN 14

/** 帧内 4 字节小端读取。 */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

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

/** 出厂数据区：序列号、VID/PID、机身配色与摇杆校准（controller.md §7.2）。
 * 校准取中位 2048、行程 ±2047/2048，与编码器 0-4095 直发语义保持 1:1。 */
static void factory_init(void)
{
    memset(s_factory, 0xFF, sizeof(s_factory));
    memcpy(&s_factory[0x0002], "REMAPAD-S3-0001", 15);
    s_factory[0x0011] = 0x00;
    s_factory[0x0012] = 0x7E;
    s_factory[0x0013] = 0x05;
    s_factory[0x0014] = (uint8_t)(NS2_PID_PRO_CONTROLLER_2 & 0xFF);
    s_factory[0x0015] = (uint8_t)(NS2_PID_PRO_CONTROLLER_2 >> 8);
    s_factory[0x0019] = 0xE8;
    s_factory[0x001A] = 0x5D;
    s_factory[0x001B] = 0x22;
    s_factory[0x001C] = 0x3C;
    s_factory[0x001D] = 0x3C;
    s_factory[0x001E] = 0x3C;
    s_factory[0x001F] = 0xF0;
    s_factory[0x0020] = 0xF0;
    s_factory[0x0021] = 0xF0;
    s_factory[0x0022] = 0x2E;
    s_factory[0x0023] = 0x2E;
    s_factory[0x0024] = 0x2E;
    static const uint8_t stick_cal[9] = {0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80};
    memcpy(&s_factory[0x00A8], stick_cal, sizeof(stick_cal));
    memcpy(&s_factory[0x00E8], stick_cal, sizeof(stick_cal));

    /* 0x7E40 出厂块按实机抓包布局：6B 头 + 14B 序列号 + 2B 保留 +
     * 4B VID/PID + 3B 版本 + 12B 机身配色，尾部未用区 0xFF。 */
    memset(s_factory_block, 0xFF, sizeof(s_factory_block));
    s_factory_block[0] = 0x00;
    s_factory_block[1] = 0x30;
    s_factory_block[2] = 0x01;
    s_factory_block[3] = 0x00;
    s_factory_block[4] = 0x01;
    s_factory_block[5] = 0x00;
    memcpy(&s_factory_block[6], "REMAPAD2S30001", 14);
    s_factory_block[22] = 0x7E;
    s_factory_block[23] = 0x05;
    s_factory_block[24] = 0x69;
    s_factory_block[25] = 0x20;
    s_factory_block[26] = 0x01;
    s_factory_block[27] = 0x06;
    s_factory_block[28] = 0x01;
    static const uint8_t body_colors[12] = {
        0x23, 0x23, 0x23, 0xA0, 0xA0, 0xA0, 0xE6, 0xE6, 0xE6, 0x32, 0x32, 0x32,
    };
    memcpy(&s_factory_block[29], body_colors, sizeof(body_colors));
}

/** 配对区镜像：1B 数量 + 40B 记录项（MAC@+0x08、LTK@+0x1A，controller.md §7.4）。 */
static void pairing_region_image(uint8_t out[PAIRING_IMAGE_SIZE])
{
    memset(out, 0x00, PAIRING_IMAGE_SIZE);
    const size_t count = ble_creds_count();
    out[0] = (uint8_t)count;
    for (size_t i = 0; i < count && i < 5; i++) {
        const ns2_cred_record_t *rec = ble_creds_get(i);
        memcpy(&out[0x08 + i * 0x28], rec->mac, 6);
        memcpy(&out[0x1A + i * 0x28], rec->ltk, 16);
    }
}

/** SPI Flash 读取仿真：出厂数据区与配对区有效；0x7E40 出厂块按实机抓包
 * 布局返回，其余地址按未初始化 0xFF。 */
static void flash_read(uint32_t addr, uint8_t *out, size_t len)
{
    if (addr >= 0x7E00u && (uint64_t)addr + len <= 0x7E80u) {
        for (size_t i = 0; i < len; i++) {
            const uint32_t a = addr + (uint32_t)i;
            out[i] = (a >= 0x7E40u && a < 0x7E80u) ? s_factory_block[a - 0x7E40u] : 0xFF;
        }
        return;
    }
    if (addr >= PAIRING_BASE && (uint64_t)addr + len <= PAIRING_BASE + 0x1000u) {
        uint8_t image[PAIRING_IMAGE_SIZE];
        pairing_region_image(image);
        for (size_t i = 0; i < len; i++) {
            const uint32_t offset = addr + (uint32_t)i - PAIRING_BASE;
            out[i] = offset < PAIRING_IMAGE_SIZE ? image[offset] : 0xFF;
        }
        return;
    }
    if (addr >= FACTORY_BASE && (uint64_t)addr + len <= FACTORY_BASE + FACTORY_SIZE) {
        memcpy(out, &s_factory[addr - FACTORY_BASE], len);
        return;
    }
    memset(out, 0xFF, len);
}

/** 31 字节手柄广播载荷（controller.md §2.1）：Flags 3B + 厂商数据 28B。
 * 偏移：5-6 Company ID、7-9 协议头、10-11 VID、12-13 PID、16 状态位、
 * 17-22 目标主机 MAC 反序、23 尾部标志。有凭证时构造回连广播。 */
static void build_adv_payload(uint8_t out[31], bool reconnect)
{
    static const uint8_t tpl[31] = {
        0x02, 0x01, 0x06,
        0x1B, 0xFF,
        0x53, 0x05, 0x01, 0x00, 0x03,
        0x7E, 0x05,
        0x69, 0x20,
        0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(out, tpl, 31);
    if (reconnect && ble_creds_count() > 0) {
        const ns2_cred_record_t *rec = ble_creds_get(0);
        memcpy(&out[17], rec->mac, 6);
        s_ses.state = SESSION_ADV_RECONNECT;
    } else {
        s_ses.state = SESSION_ADV_DISCOVERY;
    }
}

/** 按凭证状态恢复广播：已配对发回连广播等待主机；未配对保持静默，
 * 进入配对模式（UI「开始」）才发发现广播——与真实手柄行为一致。 */
static void resume_advertising(void)
{
    if (ble_creds_count() > 0) {
        uint8_t adv[31];
        build_adv_payload(adv, true);
        ble_controller_advertise(adv);
    } else {
        ble_controller_adv_stop();
        s_ses.state = SESSION_ADV_DISCOVERY;
    }
}

void ns2_session_on_sync(const uint8_t own_mac[6])
{
    factory_init();
    memcpy(s_ses.own_mac, own_mac, 6);
    ESP_LOGI(TAG, "host synced, own MAC %02x:%02x:%02x:%02x:%02x:%02x",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
    resume_advertising();
}

void ns2_session_on_connect(uint16_t conn_handle)
{
    uint8_t peer[6];
    bool known = false;
    if (ble_controller_peer_mac(conn_handle, peer)) {
        for (size_t i = 0; i < ble_creds_count(); i++) {
            if (memcmp(ble_creds_get(i)->mac, peer, 6) == 0) {
                known = true;
                break;
            }
        }
    }
    s_ses.state = known ? SESSION_NORMAL : SESSION_CONNECTED_WAIT_PAIR;
    s_ses.pairing_mode = false;
    ESP_LOGI(TAG, "connected (conn=%u, %s), waiting host init sequence",
             conn_handle, known ? "paired host" : "unpaired host");
}

void ns2_session_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected, resume advertising");
    resume_advertising();
}

/** Command 0x02 SPI Flash 读取：0x01 固定 64B 块、0x04 通用读取（controller.md §6.2）。 */
static size_t handle_flash_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp, size_t cap)
{
    if (subcmd == 0x01) {
        if (len < NS2_FRAME_HEADER_LEN + 8 || cap < 8 + 72) {
            return 0;
        }
        const uint32_t addr = le32(&req[12]);
        uint8_t data[64];
        flash_read(addr, data, sizeof(data));
        return NS2_FRAME_HEADER_LEN +
               ns2_body_flash_read(&resp[8], cap - 8, addr, data, sizeof(data));
    }
    if (subcmd == 0x04) {
        if (len < NS2_FRAME_HEADER_LEN + 8 || cap < 8 + 72) {
            return 0;
        }
        size_t rlen = req[8];
        const uint32_t addr = le32(&req[12]);
        if (rlen > 64) {
            rlen = 64;
        }
        uint8_t data[64];
        flash_read(addr, data, rlen);
        return NS2_FRAME_HEADER_LEN +
               ns2_body_flash_read(&resp[8], cap - 8, addr, data, rlen);
    }
    /* 写入/擦除子命令：本设备无用户可写仿真区，直接确认。 */
    return NS2_FRAME_HEADER_LEN;
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
    return NS2_FRAME_HEADER_LEN;
}

/** Command 0x0C 特性掩码：body 首字节生效（bit5 触觉影响 0x09 状态标志）。 */
static size_t handle_feature_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp)
{
    const uint8_t mask = len >= 12 ? req[8] : 0;
    switch (subcmd) {
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
        /* 步骤 2：请求体 `00 [16B 主机公钥 A1 反序]`；LTK = A1 XOR B1（B1 为
         * 固定公钥），应答体 `01 [16B B1]`。 */
        if (len < NS2_FRAME_HEADER_LEN + 17 || cap < NS2_FRAME_HEADER_LEN + 17) {
            return 0;
        }
        for (int i = 0; i < 16; i++) {
            s_pair.ltk[i] = (uint8_t)(req[9 + i] ^ ns2_pair_pubkey_b1[i]);
        }
        s_pair.ltk_ready = true;
        resp[8] = 0x01;
        memcpy(&resp[9], ns2_pair_pubkey_b1, 16);
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x02:
        /* 步骤 3：请求体 `00 [16B 挑战码 A2 反序]`；
         * B2 = reverse(AES128_ECB(Key=reverse(LTK), Data=reverse(A2)))，
         * 应答体 `01 [16B B2 反序]`。 */
        if (!s_pair.ltk_ready || len < NS2_FRAME_HEADER_LEN + 17 ||
            cap < NS2_FRAME_HEADER_LEN + 17) {
            return 0;
        }
        {
            uint8_t a2[16];
            uint8_t key[16];
            uint8_t b2_reversed[16];
            reverse_bytes(&req[9], a2, 16);
            reverse_bytes(s_pair.ltk, key, 16);
            if (!aes_ecb_block(key, a2, b2_reversed)) {
                ESP_LOGE(TAG, "psa aes failed");
                return 0;
            }
            resp[8] = 0x01;
            reverse_bytes(b2_reversed, &resp[9], 16);
        }
        return NS2_FRAME_HEADER_LEN + 17;
    case 0x03:
        /* 步骤 4：确认并持久化主机 MAC + LTK。 */
        if (s_pair.mac_ready && s_pair.ltk_ready) {
            ble_creds_save(s_pair.host_mac, s_pair.ltk);
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

    uint8_t resp[ANSWER_PREFIX_LEN + 96];
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
    case 0x0A:
        ESP_LOGI(TAG, "haptic sample 0x%02x", subcmd == 0x02 && len >= 9 ? data[8] : subcmd);
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
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
     * 板卡无震动马达；M5 起原样经 USB OUT 转发给源手柄。 */
    if (len == 0) {
        return;
    }
    ESP_LOGI(TAG, "rumble report %uB:", (unsigned)len);
    ESP_LOG_BUFFER_HEX(TAG, data, len);
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

void ns2_session_unpair(void)
{
    ble_creds_clear();
    /* 凭证清空后回连广播失效，恢复为未配对静默。 */
    resume_advertising();
    ESP_LOGI(TAG, "pairing credentials cleared");
}

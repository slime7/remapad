#include "ble_session.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ble_controller.h"
#include "ns2_frames.h"
#include "ns2_report.h"

static const char *TAG = "remapad_blses";

/** 手柄身份：Pro Controller 2（controller.md §1）。 */
#define NS2_PID_PRO_CONTROLLER_2 0x2069

/** 会话状态：休眠/唤醒广播顺延到后续里程碑（见 docs/ROADMAP.md M3）。 */
typedef enum {
    SESSION_ADV_DISCOVERY = 0,
    SESSION_CONNECTED_WAIT_PAIR,
    SESSION_NORMAL,
} session_state_t;

/** 出厂数据区仿真基址（controller.md §7.2）；配对区 0x1FA000 在 M3 接入 NVS。 */
#define FACTORY_BASE 0x013000u
#define FACTORY_SIZE 2048u

static struct {
    session_state_t state;
    uint8_t own_mac[6];
    uint8_t report_format;
    uint8_t feature_mask;
    uint8_t player_leds;
} s_ses;

static uint8_t s_factory[FACTORY_SIZE];

/** 帧内 4 字节小端读取。 */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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
}

/** SPI Flash 读取仿真：仅出厂数据区有效，其余地址按未初始化返回 0xFF。 */
static void flash_read(uint32_t addr, uint8_t *out, size_t len)
{
    if (addr >= FACTORY_BASE && (uint64_t)addr + len <= FACTORY_BASE + FACTORY_SIZE) {
        memcpy(out, &s_factory[addr - FACTORY_BASE], len);
        return;
    }
    memset(out, 0xFF, len);
}

/** 31 字节手柄广播载荷（controller.md §2.1）：Flags 3B + 厂商数据 28B。
 * 偏移：5-6 Company ID、7-9 协议头、10-11 VID、12-13 PID、16 状态位、
 * 17-22 目标主机 MAC 反序、23 尾部标志。 */
static void build_adv_payload(uint8_t out[31])
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
}

void ns2_session_on_sync(const uint8_t own_mac[6])
{
    factory_init();
    memcpy(s_ses.own_mac, own_mac, 6);
    ESP_LOGI(TAG, "host synced, own MAC %02x:%02x:%02x:%02x:%02x:%02x",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);
    uint8_t adv[31];
    build_adv_payload(adv);
    s_ses.state = SESSION_ADV_DISCOVERY;
    ble_controller_advertise(adv);
}

void ns2_session_on_connect(uint16_t conn_handle)
{
    s_ses.state = SESSION_CONNECTED_WAIT_PAIR;
    ESP_LOGI(TAG, "connected (conn=%u), waiting host init sequence", conn_handle);
}

void ns2_session_on_disconnect(void)
{
    s_ses.state = SESSION_ADV_DISCOVERY;
    ESP_LOGI(TAG, "disconnected, resume discovery advertising");
    uint8_t adv[31];
    build_adv_payload(adv);
    ble_controller_advertise(adv);
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

/** Command 0x03 初始化与连接建立（controller.md §6.2）；0x07/0x08/0x09 配对注入在 M3 落地。 */
static size_t handle_init_cmd(const uint8_t *req, size_t len, uint8_t subcmd, uint8_t *resp)
{
    switch (subcmd) {
    case 0x01:
        ESP_LOGI(TAG, "wake advertising request=%u (M3 pending)", req[8]);
        return NS2_FRAME_HEADER_LEN;
    case 0x0A:
        if (len >= 9 && (req[8] == NS2_REPORT_ID_05 || req[8] == NS2_REPORT_ID_09)) {
            s_ses.report_format = req[8];
            ESP_LOGI(TAG, "input report format -> 0x%02x", req[8]);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x07:
        if (len >= 14) {
            ESP_LOGI(TAG, "pairing inject (M3 pending): %02x:%02x:%02x:%02x:%02x:%02x",
                     req[8], req[9], req[10], req[11], req[12], req[13]);
        }
        return NS2_FRAME_HEADER_LEN;
    case 0x08:
        ESP_LOGI(TAG, "pairing clear (M3 pending)");
        return NS2_FRAME_HEADER_LEN;
    case 0x09:
        ESP_LOGI(TAG, "pairing save (M3 pending)");
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

void ns2_session_on_command(const uint8_t *data, size_t len, uint8_t transport)
{
    if (len < NS2_FRAME_HEADER_LEN) {
        ESP_LOGW(TAG, "command too short (%u)", (unsigned)len);
        return;
    }
    const uint8_t cmd = data[0];
    const uint8_t subcmd = data[3];

    uint8_t resp[96];
    size_t resp_len;
    switch (cmd) {
    case 0x07:
        /* 初始握手（§10.2 阶段 1）：应答体 1 字节 0x00。 */
        resp[8] = 0x00;
        resp_len = NS2_FRAME_HEADER_LEN + 1;
        break;
    case NS2_CMD_SPI_FLASH:
        resp_len = handle_flash_cmd(data, len, subcmd, resp, sizeof(resp));
        break;
    case 0x03:
        resp_len = handle_init_cmd(data, len, subcmd, resp);
        break;
    case 0x09:
        resp_len = handle_led_cmd(data, len, subcmd);
        break;
    case 0x0A:
        ESP_LOGI(TAG, "haptic sample 0x%02x", subcmd == 0x02 && len >= 9 ? data[8] : subcmd);
        resp_len = NS2_FRAME_HEADER_LEN;
        break;
    case 0x0C:
        resp_len = handle_feature_cmd(data, len, subcmd, resp);
        break;
    case NS2_CMD_VERSION:
        if (subcmd == 0x01) {
            ns2_body_version(&resp[8]);
            resp_len = NS2_FRAME_HEADER_LEN + NS2_VERSION_BODY_LEN;
        } else {
            resp_len = NS2_FRAME_HEADER_LEN;
        }
        break;
    case NS2_CMD_PAIRING:
        ESP_LOGI(TAG, "pairing step 0x%02x received (M3 pending)", subcmd);
        resp_len = NS2_FRAME_HEADER_LEN;
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
    ns2_frame_response_header(resp, cmd, transport, subcmd);
    ble_controller_notify_answer(resp, resp_len);
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
    if (len < 32) {
        ESP_LOGW(TAG, "composite too short (%u)", (unsigned)len);
        return;
    }
    ns2_session_on_output(data, 32);
    if (len > 32) {
        ns2_session_on_command(&data[32], len - 32, NS2_FRAME_TRANSPORT_BLE);
    }
}

uint8_t ns2_session_report_format(void)
{
    return s_ses.report_format;
}

bool ns2_session_rumble_enabled(void)
{
    return (s_ses.feature_mask & 0x20) != 0;
}

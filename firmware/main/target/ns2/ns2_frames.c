#include "ns2_frames.h"

#include <string.h>

#include "app_config.h"
#include "ns2_state.h"

/** 官方手柄当前固件返回的固定公钥（controller.md §3.2）。 */
const uint8_t ns2_pair_pubkey_b1[NS2_PAIR_PUBKEY_LEN] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10,
};

void ns2_frame_response_header(uint8_t out[NS2_FRAME_HEADER_LEN],
                               uint8_t cmd, uint8_t transport, uint8_t subcmd)
{
    out[0] = cmd;
    out[1] = NS2_FRAME_DIR_RESPONSE;
    out[2] = transport;
    out[3] = subcmd;
    out[4] = 0x10;
    out[5] = NS2_FRAME_ACK_OK;
    out[6] = 0x00;
    out[7] = 0x00;
}

size_t ns2_frame_response(uint8_t *out, size_t cap, uint8_t cmd, uint8_t transport,
                          uint8_t subcmd, const void *body, size_t body_len)
{
    if (cap < NS2_FRAME_HEADER_LEN + body_len) {
        return 0;
    }
    ns2_frame_response_header(out, cmd, transport, subcmd);
    if (body_len > 0) {
        memcpy(&out[NS2_FRAME_HEADER_LEN], body, body_len);
    }
    return NS2_FRAME_HEADER_LEN + body_len;
}

void ns2_body_version(uint8_t out[NS2_VERSION_BODY_LEN], uint8_t identity)
{
    /* 固件版本取持久化值（出厂值固化在 app_config.h 的 CONFIG_DEFAULT_FW_VERSION_*）；
     * 手柄类型按当前会话身份（0x00 JC L / 0x01 JC R / 0x02 Pro）；
     * BT 栈补丁 0.0.12，音频 DSP 固件未实现，按参考实现填 0xFF。 */
    const uint8_t *ver = app_config_get()->fw_version;
    out[0] = ver[0];
    out[1] = ver[1];
    out[2] = ver[2];
    out[3] = identity == NS2_ID_JOYCON_L ? 0x00u
             : identity == NS2_ID_JOYCON_R ? 0x01u
                                           : 0x02u;
    out[4] = 0x0C;
    out[5] = 0x00;
    out[6] = 0x00;
    out[7] = 0x00;
    out[8] = 0xFF;
    out[9] = 0xFF;
    out[10] = 0xFF;
    out[11] = 0xFF;
}

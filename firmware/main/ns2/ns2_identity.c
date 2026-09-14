#include "ns2_identity.h"

#include <string.h>

/** JoyCon 左右各自的固定盐：派生地址与公共地址不共用字节序列，两只之间
 *  也不只差最低位（主机按地址区分设备，近似地址可能导致识别出错）。 */
#define NS2_ADV_SALT_JOYCON_L 0x4A434C31u /* "JCL1" */
#define NS2_ADV_SALT_JOYCON_R 0x4A434C32u /* "JCL2" */

/** 32 位扩散（bit-mixer 终结器）：输入差一位，输出各字节都会不同。 */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

const char *ns2_identity_name(uint8_t identity)
{
    switch (identity) {
    case NS2_ID_PRO:
        return "pro";
    case NS2_ID_JOYCON_L:
        return "jc-l";
    case NS2_ID_JOYCON_R:
        return "jc-r";
    default:
        return "?";
    }
}

void ns2_identity_adv_addr(const uint8_t own_mac[6], uint8_t identity, uint8_t out[6])
{
    memcpy(out, own_mac, 6);
    if (identity == NS2_ID_PRO) {
        return;
    }
    const uint32_t salt = identity == NS2_ID_JOYCON_R ? NS2_ADV_SALT_JOYCON_R
                                                     : NS2_ADV_SALT_JOYCON_L;
    const uint32_t low = (uint32_t)own_mac[0] | ((uint32_t)own_mac[1] << 8) |
                         ((uint32_t)own_mac[2] << 16);
    const uint32_t high = (uint32_t)own_mac[3] | ((uint32_t)own_mac[4] << 8) |
                          ((uint32_t)own_mac[5] << 16);
    const uint32_t mixed_low = mix32(low ^ salt);
    const uint32_t mixed_high = mix32(high ^ salt);
    out[0] = (uint8_t)(mixed_low & 0xFF);
    out[1] = (uint8_t)((mixed_low >> 8) & 0xFF);
    out[2] = (uint8_t)((mixed_low >> 16) & 0xFF);
    out[3] = (uint8_t)(mixed_high & 0xFF);
    out[4] = (uint8_t)((mixed_high >> 8) & 0xFF);
    out[5] = (uint8_t)((mixed_high >> 16) & 0xFF);
    /* 静态随机地址形态：最高字节置 bit7/bit6；左右以最低位区分（本机地址
     * 最低位来自芯片，奇偶不定，必须显式清零，右只显式置一）。 */
    out[5] = (uint8_t)(out[5] | 0xC0);
    out[0] = identity == NS2_ID_JOYCON_R ? (uint8_t)(out[0] | 0x01)
                                         : (uint8_t)(out[0] & 0xFE);
}

void ns2_mac_to_string(const uint8_t mac[6], char out[18])
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < 6; i++) {
        const uint8_t byte = mac[5 - i]; /* 存储序反转即显示序。 */
        const size_t at = i * 3;
        out[at] = hex[(byte >> 4) & 0x0F];
        out[at + 1] = hex[byte & 0x0F];
        out[at + 2] = i < 5 ? ':' : '\0';
    }
}

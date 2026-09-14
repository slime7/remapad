/**
 * 身份与对外地址（ns2_identity.c）：Pro 与两只 JoyCon 的对外地址必须互不
 * 共用字节序列（不能只是「公共地址换个最高位」或「两只只差最低位」），且都
 * 带静态随机形态的最高位；这一条错了会让主机把同一台设备的两种形态认成一台，
 * 或者把两只 JoyCon 认成一只。地址的最低位来自芯片，奇偶不定，奇数地址也覆盖。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_identity.h"

static void identity_names(void)
{
    CHECK(strcmp(ns2_identity_name(NS2_ID_PRO), "pro") == 0);
    CHECK(strcmp(ns2_identity_name(NS2_ID_JOYCON_L), "jc-l") == 0);
    CHECK(strcmp(ns2_identity_name(NS2_ID_JOYCON_R), "jc-r") == 0);
    CHECK(strcmp(ns2_identity_name(0x7F), "?") == 0);
}

/** 两个地址中不一样的字节数。 */
static size_t byte_diff(const uint8_t a[6], const uint8_t b[6])
{
    size_t diff = 0;
    for (size_t i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            diff++;
        }
    }
    return diff;
}

static void adv_addr_derivation(void)
{
    /* 两样本机地址：最低字节偶 / 奇各一，覆盖芯片差异。 */
    const uint8_t own_even[6] = {0x3C, 0x2B, 0x1A, 0x8C, 0x81, 0x78};
    const uint8_t own_odd[6] = {0x3D, 0x2B, 0x1A, 0x8C, 0x81, 0x78};

    for (size_t sample = 0; sample < 2; sample++) {
        const uint8_t *own = sample == 0 ? own_even : own_odd;
        uint8_t pro[6];
        uint8_t left[6];
        uint8_t right[6];
        uint8_t again[6];

        ns2_identity_adv_addr(own, NS2_ID_PRO, pro);
        CHECK_BYTES(pro, own, 6);

        ns2_identity_adv_addr(own, NS2_ID_JOYCON_L, left);
        ns2_identity_adv_addr(own, NS2_ID_JOYCON_R, right);

        /* 静态随机形态：最高字节高两位为 1；左右最低位 0 / 1。 */
        CHECK_EQ(left[5] & 0xC0, 0xC0);
        CHECK_EQ(right[5] & 0xC0, 0xC0);
        CHECK_EQ(left[0] & 0x01, 0x00);
        CHECK_EQ(right[0] & 0x01, 0x01);

        /* 与公共地址、左右彼此之间都不共用字节序列：不能是「同一地址只换
         * 最高位」或「两只只差最低位」这种近似形态，主机按地址区分设备。 */
        CHECK(byte_diff(left, own) >= 3);
        CHECK(byte_diff(right, own) >= 3);
        CHECK(byte_diff(left, right) >= 3);

        /* 可重复派生：同一芯片每次算出的地址一致。 */
        ns2_identity_adv_addr(own, NS2_ID_JOYCON_L, again);
        CHECK_BYTES(again, left, 6);
    }
}

static void mac_string_format(void)
{
    const uint8_t mac[6] = {0x3C, 0x2B, 0x1A, 0x8C, 0x81, 0x78};
    char text[18];
    ns2_mac_to_string(mac, text);
    CHECK(strcmp(text, "78:81:8C:1A:2B:3C") == 0);
    CHECK_EQ(strlen(text), 17);

    /* 全 0 与全 0xFF：低位补零、字母大写。 */
    const uint8_t zeros[6] = {0};
    ns2_mac_to_string(zeros, text);
    CHECK(strcmp(text, "00:00:00:00:00:00") == 0);
    const uint8_t ones[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ns2_mac_to_string(ones, text);
    CHECK(strcmp(text, "FF:FF:FF:FF:FF:FF") == 0);
}

HOST_TEST_SUITE(suite_ns2_identity, "ns2_identity",
                {"身份短名", identity_names},
                {"对外地址派生：与公共地址、左右之间都不共用字节序列", adv_addr_derivation},
                {"地址字符串：显示序大写十六进制", mac_string_format});

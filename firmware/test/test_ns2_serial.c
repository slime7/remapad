/**
 * NS2 序列号命名规则（ns2_serial.c）主机端用例：前缀、地区码与校验位逐条钉住，
 * 并拿文档里的真实示例当黄金样本；校验位错一位主机可能不认这台手柄，日志里完全看不出来。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_serial.h"

/** 按文档公式独立重算校验位，用来对照被测实现。 */
static char reference_check_digit(const char *digits10)
{
    int sum = 0;
    for (int index = 0; index < 10; index++) {
        const int digit = digits10[index] - '0';
        sum += (index % 2 == 0) ? digit : 3 * digit;
    }
    return (char)('0' + ((10 - sum % 10) % 10));
}

static void builds_documented_samples(void)
{
    char serial[15];
    ns2_serial_build("HEJ", "7100112124", serial);
    CHECK_EQ(strcmp(serial, "HEJ71001121247"), 0);

    ns2_serial_build("HEJ", "7100112345", serial);
    CHECK_EQ(strcmp(serial, "HEJ71001123456"), 0);
}

static void builds_serials_shown_in_ui(void)
{
    /* 与 ui/src/pages/ControllerSettingsPage.tsx 展示的占位序列号一致：
     * 固件的出厂块与界面文案是同一条规则算出来的，不能各写各的。 */
    char serial[15];
    ns2_serial_build("HEJ", "7100112345", serial);
    CHECK_EQ(strcmp(serial, "HEJ71001123456"), 0);

    ns2_serial_build("HBW", "1006701234", serial);
    CHECK_EQ(strcmp(serial, "HBW10067012342"), 0);

    ns2_serial_build("HCW", "1006801234", serial);
    CHECK_EQ(strcmp(serial, "HCW10068012341"), 0);
}

static void shape_and_prefix(void)
{
    char serial[15];
    memset(serial, 0x7F, sizeof(serial));
    ns2_serial_build("ABC", "0123456789", serial);
    CHECK_EQ(strlen(serial), 14);
    CHECK_EQ(serial[14], '\0');
    CHECK(serial[0] == 'A' && serial[1] == 'B' && serial[2] == 'C');
    CHECK(memcmp(&serial[3], "0123456789", 10) == 0);
    CHECK_EQ(serial[13], reference_check_digit("0123456789"));
}

static void check_digit_covers_edge_sums(void)
{
    static const char *const samples[] = {
        "0000000000", /* 和为 0：校验位 0 */
        "0900000000", /* 奇位 9：3 × 9 = 27，校验位 3 */
        "0055000000", /* 偶位 5 与奇位 5 组合 */
        "9999999999", /* 最大和，校验位回到 0 */
        "1234567890",
        "0000000009",
        "9090909090",
    };
    char serial[15];
    for (size_t index = 0; index < sizeof(samples) / sizeof(samples[0]); index++) {
        ns2_serial_build("HAA", samples[index], serial);
        CHECK_EQ(serial[13], reference_check_digit(samples[index]));
        /* 校验位必须是数字，拼接进 14 位后长度不变。 */
        CHECK(serial[13] >= '0' && serial[13] <= '9');
        CHECK_EQ(strlen(serial), 14);
    }
}

HOST_TEST_SUITE(suite_ns2_serial, "ns2_serial",
                {"文档给出的两个示例序列号", builds_documented_samples},
                {"界面展示的占位序列号同一规则", builds_serials_shown_in_ui},
                {"14 位形状与前缀原样拷贝", shape_and_prefix},
                {"校验位覆盖边界和值", check_digit_covers_edge_sums});

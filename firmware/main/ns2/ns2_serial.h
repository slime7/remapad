#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NS2 序列号命名规则（controller.md §7.2）：14 位 ASCII = 3 字母前缀 +
 * 11 位数字，末位为校验位。前缀首字母 H 为 Switch 2 代际；次字母为硬件
 * 型号（A 主机、B Joy-Con 2 (L)、C Joy-Con 2 (R)、E Pro Controller 2）；
 * 第三字母为销售地区（J 日本、W 美洲…）。校验位对前缀后的前 10 位数字
 * 计算：S = 偶位和（0 基）+ 3 × 奇位和，校验位 = (10 − S mod 10) mod 10
 * （文档示例 HEJ71001121247 / HEJ71001123456 均自洽）。
 */

/** 生成 14 位序列号（out 为 15 字节含 NUL）。digits10 为前缀后的 10 位
 * 数字 ASCII，末位校验位在此补齐。 */
void ns2_serial_build(const char prefix[3], const char digits10[10], char out[15]);

#ifdef __cplusplus
}
#endif

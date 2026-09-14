#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 手柄身份的对外地址与命名（纯逻辑，无 IDF 依赖，可主机端测试）。
 * Pro 单身份对外用公共伪装地址；JoyCon 组合的左右两只各占一个广播实例，
 * 各自派生静态随机地址，两只地址必然不同。
 */

/** 身份短名（日志与诊断用）：pro / jc-l / jc-r；未知身份返回 "?"。 */
const char *ns2_identity_name(uint8_t identity);

/** 按身份派生对外广播地址（NimBLE 存储序，即显示序反转）：Pro 原样返回
 *  own_mac；JoyCon 两侧用各自的固定盐把 own_mac 扩散成另一串字节，再置成
 *  静态随机形态（最高字节 bit7/bit6 置一，最低位左清零右置一）。
 *  派生结果与公共地址、以及左右彼此之间都不共用字节序列：同一台设备在
 *  Pro 形态与 JoyCon 形态下必须是主机眼中完全不同的两台设备，两只 JoyCon
 *  也不能只差最低位。同一芯片上结果稳定（可重复派生）。 */
void ns2_identity_adv_addr(const uint8_t own_mac[6], uint8_t identity, uint8_t out[6]);

/** 地址格式化为显示序大写十六进制（"AA:BB:CC:DD:EE:FF"），out 至少 18 字节。 */
void ns2_mac_to_string(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif

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
 * 设备对外只有一台 Pro Controller 2，广播一律用公共伪装地址——主机只接受
 * public 地址的广播（2026-09-16 实机对账，见 docs/controller.md §12）。
 */

/** 身份短名（日志与诊断用）：pro；未知身份返回 "?"。 */
const char *ns2_identity_name(uint8_t identity);

/** 广播地址形态（实机对账开关，见 ns2_session_set_adv_addr_form）：
 *  auto 与 public 都是公共伪装地址，random 换成派生静态随机地址做对照。 */
typedef enum {
    NS2_ADV_ADDR_AUTO = 0,
    NS2_ADV_ADDR_PUBLIC = 1,
    NS2_ADV_ADDR_RANDOM = 2,
} ns2_adv_addr_form_t;

/** 形态短名（日志与串口回显）：auto / public / random。 */
const char *ns2_adv_addr_form_name(uint8_t form);

/** 派生静态随机广播地址（只给串口 advaddr random 的对账开关用，NimBLE 存储序）：
 *  用固定盐把 own_mac 扩散成另一串字节，再置成静态随机形态（最高字节
 *  bit7/bit6 置一、最低位清零）。派生结果与公共地址不共用字节序列，同一芯片
 *  上结果稳定（可重复派生）。 */
void ns2_identity_adv_addr_random(const uint8_t own_mac[6], uint8_t out[6]);

/** 地址格式化为显示序大写十六进制（"AA:BB:CC:DD:EE:FF"），out 至少 18 字节。 */
void ns2_mac_to_string(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif

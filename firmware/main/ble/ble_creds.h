#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 配对凭证持久化（ADR 0009：存 NVS），按手柄身份分槽（ns2_identity_t）：
 * Pro / JoyCon L / JoyCon R 各自独立的记录列表。切换手柄类型后主机眼中是
 * 另一台设备（序列号、PID、广播地址都不同），配对信息必须分开保存，未配
 * 对的身份进发现广播重新配对。记录语义对齐 controller.md §7.4 配对区（主
 * 机 MAC + 16B LTK），MAC/LTK 均按配对指令线格式原样存储（反向字节序形
 * 态），回连广播直接使用存储值。
 */

#define NS2_CREDS_MAX 4
#define NS2_CREDS_MAC_LEN 6
#define NS2_CREDS_LTK_LEN 16

typedef struct {
    uint8_t mac[NS2_CREDS_MAC_LEN];
    uint8_t ltk[NS2_CREDS_LTK_LEN];
} ns2_cred_record_t;

/** 从 NVS 装载凭证到内存（旧版单表记录迁移进 Pro 槽）；在
 * ble_controller_start 之前调用。 */
esp_err_t ble_creds_init(void);

/** 指定身份的记录条数。 */
size_t ble_creds_count(ns2_identity_t identity);

/** 读取指定身份的第 index 条记录；越界返回 NULL。 */
const ns2_cred_record_t *ble_creds_get(ns2_identity_t identity, size_t index);

/** 保存或更新指定身份的一条凭证（MAC 已存在则覆盖 LTK），立即写回 NVS。 */
esp_err_t ble_creds_save(ns2_identity_t identity, const uint8_t mac[NS2_CREDS_MAC_LEN],
                         const uint8_t ltk[NS2_CREDS_LTK_LEN]);

/** 清除指定身份的全部凭证并写回 NVS。 */
esp_err_t ble_creds_clear(ns2_identity_t identity);

#ifdef __cplusplus
}
#endif

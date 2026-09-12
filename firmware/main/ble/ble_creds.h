#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 配对凭证持久化（ADR 0009：存 NVS）。
 * 记录语义对齐 controller.md §7.4 配对区（主机 MAC + 16B LTK），MAC/LTK 均按
 * 配对指令线格式原样存储（反向字节序形态），回连广播直接使用存储值。
 */

#define NS2_CREDS_MAX 4
#define NS2_CREDS_MAC_LEN 6
#define NS2_CREDS_LTK_LEN 16

typedef struct {
    uint8_t mac[NS2_CREDS_MAC_LEN];
    uint8_t ltk[NS2_CREDS_LTK_LEN];
} ns2_cred_record_t;

/** 从 NVS 装载凭证到内存；在 ble_controller_start 之前调用。 */
esp_err_t ble_creds_init(void);

size_t ble_creds_count(void);

const ns2_cred_record_t *ble_creds_get(size_t index);

/** 保存或更新一条凭证（MAC 已存在则覆盖 LTK），立即写回 NVS。 */
esp_err_t ble_creds_save(const uint8_t mac[NS2_CREDS_MAC_LEN],
                         const uint8_t ltk[NS2_CREDS_LTK_LEN]);

/** 清除全部凭证并写回 NVS。 */
esp_err_t ble_creds_clear(void);

#ifdef __cplusplus
}
#endif

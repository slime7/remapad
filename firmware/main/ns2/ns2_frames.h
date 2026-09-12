#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 指令帧头长度（controller.md §6.1）。 */
#define NS2_FRAME_HEADER_LEN 8

#define NS2_FRAME_TRANSPORT_USB 0x00
#define NS2_FRAME_TRANSPORT_BLE 0x01
#define NS2_FRAME_DIR_REQUEST 0x91
#define NS2_FRAME_DIR_RESPONSE 0x01
#define NS2_FRAME_ACK_OK 0x78

/** 本阶段实现应答的命令号。 */
#define NS2_CMD_SPI_FLASH 0x02
#define NS2_CMD_VERSION 0x10
#define NS2_CMD_PAIRING 0x15

/** 手柄固定公钥 B1（controller.md §3.2），LTK = A1 XOR B1。 */
#define NS2_PAIR_PUBKEY_LEN 16
extern const uint8_t ns2_pair_pubkey_b1[NS2_PAIR_PUBKEY_LEN];

#define NS2_VERSION_BODY_LEN 12

/** 构造响应帧头：Direction 0x01、Status 0x10、ACK 0x78。 */
void ns2_frame_response_header(uint8_t out[NS2_FRAME_HEADER_LEN],
                               uint8_t cmd, uint8_t transport, uint8_t subcmd);

/** 响应帧头 + 应答体一次成型；容量不足返回 0，成功返回总长度。 */
size_t ns2_frame_response(uint8_t *out, size_t cap, uint8_t cmd, uint8_t transport,
                          uint8_t subcmd, const void *body, size_t body_len);

/** Command 0x10 应答体：固件版本 1.0.14 + Pro Controller 类型码。 */
void ns2_body_version(uint8_t out[NS2_VERSION_BODY_LEN]);

/** Command 0x02/0x04 读取应答体：1B 长度 + 3B 保留 + 4B 小端地址 + 数据。
 * 长度上限 0xFF；容量不足或超限返回 0，成功返回写入长度。 */
size_t ns2_body_flash_read(uint8_t *out, size_t cap, uint32_t addr,
                           const uint8_t *data, size_t len);

/** 配对与广播统一反向字节序：把 6B MAC 反序写入 out。 */
void ns2_body_mac_reversed(uint8_t out[6], const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

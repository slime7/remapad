#pragma once

#include <stdint.h>

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Input Report 0x05 / 0x09 的线格式体均为 63 字节；USB 模式额外前置 1 字节 Report ID。 */
#define NS2_INPUT_05_LEN 63
#define NS2_INPUT_09_LEN 63

#define NS2_REPORT_ID_05 0x05
#define NS2_REPORT_ID_09 0x09

/** 摇杆 12 位紧凑打包（controller.md §5.3）：3 字节承载 X、Y 各 12 位。 */
void ns2_pack_stick(uint16_t x, uint16_t y, uint8_t out[3]);

/** 摇杆 12 位紧凑解包，与 ns2_pack_stick 互逆。 */
void ns2_unpack_stick(const uint8_t in[3], uint16_t *x, uint16_t *y);

/** 按手柄身份切分规范化状态（JoyCon 组合的左右分摊）：Pro 原样拷贝；
 * L 保留 L 侧按键/十字键/左摇杆，R 保留 A/B/X/Y/右摇杆，NFC 状态清零。 */
void ns2_state_for_identity(ns2_controller_state_t *out,
                            const ns2_controller_state_t *in, uint8_t identity);

/** 编码 Input Report 0x09（Pro Controller 2 专用，BLE 通知体，无 Report ID）。
 * counter 为 8 位循环计数，由数据面任务维护。 */
void ns2_encode_input_09(uint8_t out[NS2_INPUT_09_LEN],
                         const ns2_controller_state_t *state, uint8_t counter);

/** 编码 Input Report 0x09 的 USB 形态：前置 Report ID 0x09。 */
void ns2_encode_input_09_usb(uint8_t out[NS2_INPUT_09_LEN + 1],
                             const ns2_controller_state_t *state, uint8_t counter);

/** 编码 Input Report 0x05（通用报告，BLE 通知体）。counter 为 32 位小端计数。 */
void ns2_encode_input_05(uint8_t out[NS2_INPUT_05_LEN],
                         const ns2_controller_state_t *state, uint32_t counter);

/** 编码 Input Report 0x05 的 USB 形态：前置 Report ID 0x05。 */
void ns2_encode_input_05_usb(uint8_t out[NS2_INPUT_05_LEN + 1],
                             const ns2_controller_state_t *state, uint32_t counter);

#ifdef __cplusplus
}
#endif

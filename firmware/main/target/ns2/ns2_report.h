#pragma once

#include <stdint.h>

#include "ns2_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Input Report 0x05 / 0x09 的线格式体均为 63 字节；USB 模式额外前置 1 字节 Report ID。 */
#define NS2_INPUT_05_LEN 63
#define NS2_INPUT_09_LEN 63

/** Report 0x09 运动数据块：长度字节固定 40（主机开启 IMU 特性位后不认
 *  长度为 0 的报文），块内字节恒为 0——板卡没有 IMU，按已验证实现填占位。 */
#define NS2_INPUT_09_MOTION_LEN 0x28

/** Report 0x09 报文体里由本机会话决定、透传时必须重写的字段偏移。 */
#define NS2_09_OFF_STATUS 0x0B
#define NS2_09_OFF_NFC 0x0C
#define NS2_09_OFF_HEADSET 0x0D
/** Report 0x09 运动块的长度字节与数据起点。 */
#define NS2_09_OFF_MOTION_LEN 0x0E
#define NS2_09_OFF_MOTION 0x0F

/** Report 0x05 的 IMU 字段：时间戳 4B + 温度 2B + 加速 XYZ + 陀螺 XYZ。 */
#define NS2_05_OFF_IMU 0x2A
#define NS2_05_IMU_LEN 18
/** 温度占位值（0.01℃ 单位，3000 = 30.00℃，输入设备不提供温度）。 */
#define NS2_05_IMU_TEMP 0x0BB8

#define NS2_REPORT_ID_05 0x05
#define NS2_REPORT_ID_09 0x09

/** 0x09 运动块里一份样本的字节数（陀螺 XYZ + 加速 XYZ，各 16 位小端）。 */
#define NS2_09_MOTION_SAMPLE_LEN 12
/** 实验模式一次填入的样本份数（余下字节补 0）。 */
#define NS2_09_MOTION_SAMPLES 3

/** 摇杆 12 位紧凑打包（controller.md §5.3）：3 字节承载 X、Y 各 12 位。 */
void ns2_pack_stick(uint16_t x, uint16_t y, uint8_t out[3]);

/** 摇杆 12 位紧凑解包，与 ns2_pack_stick 互逆。 */
void ns2_unpack_stick(const uint8_t in[3], uint16_t *x, uint16_t *y);

/** 按手柄身份切分规范化状态（JoyCon 组合的左右分摊）：Pro 原样拷贝；
 * L 保留 L 侧按键/十字键/左摇杆，R 保留 A/B/X/Y/C/右摇杆，NFC 状态
 * 只在 R 半边保留（右手柄才有 NFC 硬件）。 */
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

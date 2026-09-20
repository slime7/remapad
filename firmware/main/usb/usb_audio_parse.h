#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 找到的音频流（AS）输出接口：Type I PCM 16 位、带等时 OUT 端点的最低
 *  非 0 备用设置。DualSense 的音频布局固定为 48kHz / 4ch（实机
 *  描述符抓包：接口 1 alt 1、端点 0x01、MPS 392 = 49 样本 × 4ch × 2B）。 */
typedef struct {
    uint8_t iface;
    uint8_t alt;
    uint8_t ep_addr;
    uint16_t ep_mps;
    uint8_t channels;
    uint8_t subframe_size;
    uint8_t bit_resolution;
    uint32_t sample_rate_hz;
} usb_audio_as_out_t;

/**
 * 在完整配置描述符里找 UAC1 的音频流 OUT 接口：跳过音频控制接口、麦克风流
 * （IN 端点）与 HID 接口，要求 AS_GENERAL 声明 PCM（wFormatTag 0x0001）、
 * FORMAT_TYPE_I 给出通道数与采样率。找不到（设备没有可驱动的音频通道或格式
 * 不合）返回 false。只读描述符，不做任何传输，可在主机端测试。
 */
bool usb_audio_find_as_out(const uint8_t *config_desc, size_t len, usb_audio_as_out_t *out);

#ifdef __cplusplus
}
#endif

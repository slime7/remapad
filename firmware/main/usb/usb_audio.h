#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "haptic_synth.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DS5 音频触觉通道（USB host 直插）：DualSense 的 USB 音频接口是 UAC1 的 48kHz / 4ch PCM，
 * 后两路直接驱动左右触觉音圈。只认布局行声明了音频触觉能力的设备：claim 音频流 OUT 接口的
 * 非 0 备用设置并持续送合成 PCM，震动参数经 usb_audio_haptic 随到随换；HID 震动字节由数据面让位。
 */
bool usb_audio_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cfg,
                      uint16_t vid, uint16_t pid);

/** 停流并释放音频接口（拔线 / 角色切换 / 关机时随 close_device 调用）。 */
void usb_audio_detach(void);

/** 音频触觉流是否在跑（数据面据此决定 HID 震动让不让位）。 */
bool usb_audio_streaming(void);

/** 更新合成参数（振幅/频率/采样脉冲），内部拷贝，可在任意任务上下文调用。 */
void usb_audio_haptic(const haptic_synth_params_t *params);

#ifdef __cplusplus
}
#endif

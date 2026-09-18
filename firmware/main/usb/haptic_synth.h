#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** DS5 音频接口的采样布局：48kHz、四通道交错（前两路是扬声器，后两路是
 *  左右触觉音圈——通道配置 0x33 = FL+FR+RL+RR，2026-09-18 实机描述符抓包）。 */
#define HAPTIC_SYNTH_RATE_HZ 48000
#define HAPTIC_SYNTH_CHANNELS 4

/** 频率夹取范围与两带的缺省值（Hz）：参数包的频率字段单位未经实机核对，
 *  越界或为 0 时回落到缺省值，保证波形始终落在触觉音圈的有效频段。 */
#define HAPTIC_SYNTH_FREQ_MIN 20u
#define HAPTIC_SYNTH_FREQ_MAX 500u
#define HAPTIC_SYNTH_FREQ_DEFAULT_LF 55u
#define HAPTIC_SYNTH_FREQ_DEFAULT_HF 190u

/** 合成参数：两侧（左/右触觉通道）各带低频与高频的振幅（0-255，与私有反馈
 *  的归一强度同刻度）和驱动频率。pulse 是触觉采样的渲染幅度（0-255，0 =
 *  无）：采样按固件里的音色表渲染成节奏幅度后送到这里，两侧叠加高频蜂鸣，
 *  超时自灭收尾。 */
typedef struct {
    uint8_t lf_amp[2];
    uint16_t lf_freq[2];
    uint8_t hf_amp[2];
    uint16_t hf_freq[2];
    uint8_t pulse;
} haptic_synth_params_t;

/** 振荡器相位（跨块连续，换参数不重置相位，避免拼接处跳变）。 */
typedef struct {
    uint32_t phase[2][2];
    uint32_t pulse_phase;
} haptic_synth_state_t;

/** 频率落地值：0 取该带缺省，再夹到 [HAPTIC_SYNTH_FREQ_MIN, MAX]。 */
uint16_t haptic_synth_band_freq(uint16_t freq_hz, bool high_band);

void haptic_synth_reset(haptic_synth_state_t *state);

/** 生成 frames 帧四通道交错 PCM（扬声器两路恒零，触觉两路按参数合成），
 *  相位在 state 里跨块推进。 */
void haptic_synth_fill(haptic_synth_state_t *state, const haptic_synth_params_t *params,
                       int16_t *pcm, size_t frames);

#ifdef __cplusplus
}
#endif

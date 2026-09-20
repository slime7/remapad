#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DS5 音频接口的采样布局：48kHz、四通道交错（前两路是扬声器，后两路是
 *  左右触觉音圈——通道配置 0x33 = FL+FR+RL+RR，实机描述符抓包）。 */
#define HAPTIC_SYNTH_RATE_HZ 48000
#define HAPTIC_SYNTH_CHANNELS 4

/** 频率夹取范围与两带的缺省值（Hz）：NS2 的频率码已按 9 位 log2 刻度解成
 *  Hz，越界或为 0 时回落到缺省值，保证波形始终落在触觉音圈的有效频段。
 *  缺省 80/135 取 BlueRetro 驱动常量 0x180/0x1E1 的落地值。
 *  只剩旧 FEEDBACK 帧的标量频率字段还在用（布局行的 hd 规则落地时序子帧）。 */
#define HAPTIC_SYNTH_FREQ_MIN 20u
#define HAPTIC_SYNTH_FREQ_MAX 500u
#define HAPTIC_SYNTH_FREQ_DEFAULT_LF 80u
#define HAPTIC_SYNTH_FREQ_DEFAULT_HF 135u

/**
 * 合成参数（布局行 hd 规则的渲染结果 + 承载刻度）：每侧一条按时间顺序播放
 * 的子帧序列（子帧各播 slice_frames 帧），扬声器一路独立音色（采样提示音
 * 的发声段；gain 0 = 静音）。gain 为 0-255 归一刻度，amp_peak 是满幅的 PCM
 * 峰值。
 */
typedef struct {
    uint16_t amp_peak;
    uint16_t slice_frames; /**< 每个子帧的帧数（rate × cycle_ms / 1000 / 3）。 */
    pad_hd_render_t tones;
} haptic_synth_params_t;

/** 振荡器相位与子帧游标（跨块连续，换参数不重置相位，避免拼接处跳变）：
 *  每侧低频/高频各一相，扬声器另有一相；子帧游标按帧数倒数、到 0 切下一
 *  子帧（回绕），切帧不重置相位。 */
typedef struct {
    uint32_t phase[2][2];
    uint32_t speaker_phase;
    uint16_t slice_left;
    uint8_t key_index;
} haptic_synth_state_t;

/** 频率落地值：0 取该带缺省，再夹到 [HAPTIC_SYNTH_FREQ_MIN, MAX]。 */
uint16_t haptic_synth_band_freq(uint16_t freq_hz, bool high_band);

void haptic_synth_reset(haptic_synth_state_t *state);

/** 生成 frames 帧四通道交错 PCM：扬声器两路放发声音色（静音时恒零），触觉
 *  两路按各自子帧序列的当前子帧合成，相位与子帧游标在 state 里跨块推进。 */
void haptic_synth_fill(haptic_synth_state_t *state, const haptic_synth_params_t *params,
                       int16_t *pcm, size_t frames);

#ifdef __cplusplus
}
#endif

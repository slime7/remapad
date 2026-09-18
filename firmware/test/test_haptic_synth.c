/**
 * 触觉 PCM 合成（usb/haptic_synth.c）：NS2 的分带震动包络在这里变成 DS5
 * 音频通道后两路的正弦驱动。钉住三件语义：扬声器两路恒零、左右触觉通道
 * 各跟各的参数、相位跨块连续（拼接处跳变会在音圈上听成咔哒声）。
 */
#include "host_test.h"

#include <string.h>

#include "haptic_synth.h"

static int peak_of(const int16_t *pcm, size_t frames, size_t channel)
{
    int peak = 0;
    for (size_t i = 0; i < frames; i++) {
        const int v = pcm[i * HAPTIC_SYNTH_CHANNELS + channel];
        const int mag = v < 0 ? -v : v;
        if (mag > peak) {
            peak = mag;
        }
    }
    return peak;
}

static bool channel_is_silent(const int16_t *pcm, size_t frames, size_t channel)
{
    for (size_t i = 0; i < frames; i++) {
        if (pcm[i * HAPTIC_SYNTH_CHANNELS + channel] != 0) {
            return false;
        }
    }
    return true;
}

static haptic_synth_params_t params_default(void)
{
    haptic_synth_params_t params;
    memset(&params, 0, sizeof(params));
    return params;
}

static void idle_params_produce_silence(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();

    int16_t pcm[300 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 300);
    for (size_t ch = 0; ch < HAPTIC_SYNTH_CHANNELS; ch++) {
        CHECK(channel_is_silent(pcm, 300, ch));
    }

    /* 参数清零（主机断连的清理帧）后立即安静，没有衰减尾巴。 */
    params.lf_amp[0] = 255;
    params.lf_freq[0] = 48;
    haptic_synth_fill(&state, &params, pcm, 100);
    CHECK(peak_of(pcm, 100, 2) > 10000);
    params.lf_amp[0] = 0;
    haptic_synth_fill(&state, &params, pcm, 100);
    CHECK(channel_is_silent(pcm, 100, 2));
}

static void speaker_channels_stay_silent(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.lf_amp[0] = 255;
    params.lf_amp[1] = 255;
    params.hf_amp[0] = 200;
    params.hf_amp[1] = 200;

    int16_t pcm[400 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(channel_is_silent(pcm, 400, 0));
    CHECK(channel_is_silent(pcm, 400, 1));
    CHECK(peak_of(pcm, 400, 2) > 10000);
    CHECK(peak_of(pcm, 400, 3) > 10000);
}

static void sides_follow_their_own_params(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.lf_amp[0] = 255;
    params.lf_freq[0] = 48;

    int16_t pcm[1200 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1200);
    CHECK(peak_of(pcm, 1200, 2) > 20000);
    CHECK(channel_is_silent(pcm, 1200, 3));
}

static void full_amplitude_caps_at_scale(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.lf_amp[0] = 255;
    params.lf_freq[0] = 48; /* 一个周期 1000 样本，1200 样本必扫过峰值。 */

    int16_t pcm[1200 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1200);
    const int peak = peak_of(pcm, 1200, 2);
    CHECK(peak >= 23500);
    CHECK(peak <= 24050);
}

static void waveform_stays_continuous_across_fills(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.lf_amp[0] = 255;
    params.lf_freq[0] = 48;

    /* 两块拼接（模拟等时传输的逐块补数据），中途换一次强度：相位不重置，
     * 相邻样本的最大跳变不能超过同频正弦的斜率上界（24000×2π×48/48000 ≈ 151，
     * 留裕量到 400；重置相位会跳到 ~48000）。 */
    int16_t pcm[2000 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1000);
    params.lf_amp[0] = 100;
    haptic_synth_fill(&state, &params, pcm + 1000 * HAPTIC_SYNTH_CHANNELS, 1000);
    int max_jump = 0;
    for (size_t i = 1; i < 2000; i++) {
        const int a = pcm[(i - 1) * HAPTIC_SYNTH_CHANNELS + 2];
        const int b = pcm[i * HAPTIC_SYNTH_CHANNELS + 2];
        const int jump = b - a < 0 ? a - b : b - a;
        if (jump > max_jump) {
            max_jump = jump;
        }
    }
    CHECK(max_jump <= 400);
}

static void sample_pulse_buzzes_without_rumble(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.pulse = 255;

    int16_t pcm[600 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 600);
    CHECK(peak_of(pcm, 600, 2) > 15000);
    CHECK(peak_of(pcm, 600, 3) > 15000);
    CHECK(channel_is_silent(pcm, 600, 0));
    CHECK(channel_is_silent(pcm, 600, 1));
}

/** 采样脉冲按包络幅度缩放：数据面把「搜索手柄」的持续采样渲染成节奏，
 *  合成侧只吃 0-255 幅度（0 = 停顿段静默），满幅与刻度上界一致。 */
static void sample_pulse_scales_with_amplitude(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.pulse = 128;

    int16_t pcm[600 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 600);
    const int peak = peak_of(pcm, 600, 2);
    CHECK(peak >= 9900); /* 20000 × 128 / 255 ≈ 10039 */
    CHECK(peak <= 10200);

    params.pulse = 0;
    haptic_synth_fill(&state, &params, pcm, 600);
    CHECK(channel_is_silent(pcm, 600, 2));
}

static void band_freq_falls_back_and_clamps(void)
{
    CHECK_EQ(haptic_synth_band_freq(0, false), HAPTIC_SYNTH_FREQ_DEFAULT_LF);
    CHECK_EQ(haptic_synth_band_freq(0, true), HAPTIC_SYNTH_FREQ_DEFAULT_HF);
    CHECK_EQ(haptic_synth_band_freq(5, false), HAPTIC_SYNTH_FREQ_MIN);
    CHECK_EQ(haptic_synth_band_freq(600, true), HAPTIC_SYNTH_FREQ_MAX);
    CHECK_EQ(haptic_synth_band_freq(200, true), 200);
}

HOST_TEST_SUITE(suite_haptic_synth, "haptic_synth",
                {"静置参数输出纯零，清零立即安静", idle_params_produce_silence},
                {"扬声器两路恒零，触觉两路有波形", speaker_channels_stay_silent},
                {"左右触觉通道各跟各的参数", sides_follow_their_own_params},
                {"满幅强度钉在刻度上界（24000 附近）", full_amplitude_caps_at_scale},
                {"逐块补数据相位连续，换强度不跳变", waveform_stays_continuous_across_fills},
                {"采样脉冲在没有震动时也能蜂鸣", sample_pulse_buzzes_without_rumble},
                {"采样脉冲按包络幅度缩放（0 为静默）", sample_pulse_scales_with_amplitude},
                {"频率字段零回落缺省、越界夹取", band_freq_falls_back_and_clamps});

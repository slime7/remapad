#include "haptic_synth.h"

#include <string.h>

/** 256 点 Q15 正弦表（sin(2πi/256) × 32767），线性插值后误差远小于音圈
 *  的可分辨度；定点查表让完成回调里的合成只有几十次整数运算。 */
static const int16_t s_sine_lut[256] = {
       0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
    6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
   12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
   18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
   23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
   27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
   30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
   32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
   32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
   32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
   30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
   27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
   23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
   18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
   12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
    6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
       0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
   -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
  -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
  -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
  -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
  -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
  -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
  -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
  -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
  -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
  -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
  -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
  -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
  -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
  -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
   -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

/** 相位是 32 位一周期的定点角，高 8 位是查表索引、再高 8 位做插值小数。 */
static int32_t sine_q15(uint32_t phase)
{
    const uint32_t idx = phase >> 24;
    const int32_t frac = (int32_t)((phase >> 16) & 0xFFu);
    const int32_t a = s_sine_lut[idx];
    const int32_t b = s_sine_lut[(idx + 1u) & 0xFFu];
    return a + (((b - a) * frac) >> 8);
}

static uint32_t phase_step(uint16_t freq_hz)
{
    return (uint32_t)(((uint64_t)freq_hz << 32) / HAPTIC_SYNTH_RATE_HZ);
}

uint16_t haptic_synth_band_freq(uint16_t freq_hz, bool high_band)
{
    if (freq_hz == 0) {
        freq_hz = high_band ? HAPTIC_SYNTH_FREQ_DEFAULT_HF : HAPTIC_SYNTH_FREQ_DEFAULT_LF;
    }
    if (freq_hz < HAPTIC_SYNTH_FREQ_MIN) {
        return HAPTIC_SYNTH_FREQ_MIN;
    }
    if (freq_hz > HAPTIC_SYNTH_FREQ_MAX) {
        return HAPTIC_SYNTH_FREQ_MAX;
    }
    return freq_hz;
}

void haptic_synth_reset(haptic_synth_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

/** 一个振荡器的相位步进与增益（gain 0-255 × amp_peak → 正弦表乘数），静音
 *  声部直接置零增益跳过循环内的工作。 */
typedef struct {
    uint32_t step;
    int32_t gain;
} synth_osc_t;

static synth_osc_t osc_prepare(uint16_t freq_hz, uint8_t gain, uint16_t amp_peak)
{
    synth_osc_t osc;
    if (freq_hz == 0 || gain == 0) {
        osc.step = 0;
        osc.gain = 0;
        return osc;
    }
    osc.step = phase_step(freq_hz);
    osc.gain = ((int32_t)gain * amp_peak) / 255;
    if (osc.gain == 0) {
        osc.step = 0;
    }
    return osc;
}

void haptic_synth_fill(haptic_synth_state_t *state, const haptic_synth_params_t *params,
                       int16_t *pcm, size_t frames)
{
    if (state == NULL || params == NULL || pcm == NULL) {
        return;
    }
    const synth_osc_t speaker = osc_prepare(params->tones.speaker.freq,
                                            params->tones.speaker.gain, params->amp_peak);
    const uint16_t slice = params->slice_frames != 0 ? params->slice_frames : 1;
    /* 子帧游标从上一块的断点继续：循环内逐帧倒数、到 0 切下一子帧并按新
     * 子帧重算振荡器步进（切帧不重置相位，时间轴上是连续波形）。 */
    uint8_t key = state->key_index;
    uint16_t left = state->slice_left;
    if (left == 0 || key >= PAD_HD_KEY_MAX) {
        key = 0;
        left = slice;
    }
    synth_osc_t lf[2];
    synth_osc_t hf[2];
    for (size_t side = 0; side < 2; side++) {
        lf[side] = osc_prepare(params->tones.key[side][key].lf_freq,
                               params->tones.key[side][key].lf_gain, params->amp_peak);
        hf[side] = osc_prepare(params->tones.key[side][key].hf_freq,
                               params->tones.key[side][key].hf_gain, params->amp_peak);
    }
    for (size_t i = 0; i < frames; i++) {
        if (left == 0) {
            key = (uint8_t)((key + 1u) % PAD_HD_KEY_MAX);
            left = slice;
            for (size_t side = 0; side < 2; side++) {
                lf[side] = osc_prepare(params->tones.key[side][key].lf_freq,
                                       params->tones.key[side][key].lf_gain,
                                       params->amp_peak);
                hf[side] = osc_prepare(params->tones.key[side][key].hf_freq,
                                       params->tones.key[side][key].hf_gain,
                                       params->amp_peak);
            }
        }
        left--;
        int16_t *frame = &pcm[i * HAPTIC_SYNTH_CHANNELS];
        int32_t speaker_sample = 0;
        if (speaker.gain != 0) {
            speaker_sample = (sine_q15(state->speaker_phase) * speaker.gain) >> 15;
            state->speaker_phase += speaker.step;
        }
        if (speaker_sample > 32767) {
            speaker_sample = 32767;
        } else if (speaker_sample < -32768) {
            speaker_sample = -32768;
        }
        frame[0] = (int16_t)speaker_sample;
        frame[1] = (int16_t)speaker_sample;
        for (size_t side = 0; side < 2; side++) {
            int32_t sample = 0;
            if (lf[side].gain != 0) {
                sample += (sine_q15(state->phase[side][0]) * lf[side].gain) >> 15;
                state->phase[side][0] += lf[side].step;
            }
            if (hf[side].gain != 0) {
                sample += (sine_q15(state->phase[side][1]) * hf[side].gain) >> 15;
                state->phase[side][1] += hf[side].step;
            }
            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }
            frame[2 + side] = (int16_t)sample;
        }
    }
    state->key_index = key;
    state->slice_left = left;
}

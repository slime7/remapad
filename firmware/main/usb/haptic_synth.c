#include "haptic_synth.h"

#include <string.h>

/** 满幅强度（amp 255）在 16 位 PCM 上的峰值：留出两带叠加的余量。 */
#define HAPTIC_SYNTH_AMP_MAX 24000

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

void haptic_synth_fill(haptic_synth_state_t *state, const haptic_synth_params_t *params,
                       int16_t *pcm, size_t frames)
{
    if (state == NULL || params == NULL || pcm == NULL) {
        return;
    }
    /* 每侧两带的相位步进与增益先落地（频率夹取也在这里发生），循环里只剩
     * 两次查表乘移位与一次饱和叠加。 */
    uint32_t step[2][2];
    int32_t gain[2][2];
    for (size_t side = 0; side < 2; side++) {
        step[side][0] = phase_step(haptic_synth_band_freq(params->lf_freq[side], false));
        step[side][1] = phase_step(haptic_synth_band_freq(params->hf_freq[side], true));
        gain[side][0] = ((int32_t)params->lf_amp[side] * HAPTIC_SYNTH_AMP_MAX) / 255;
        gain[side][1] = ((int32_t)params->hf_amp[side] * HAPTIC_SYNTH_AMP_MAX) / 255;
    }
    for (size_t i = 0; i < frames; i++) {
        int16_t *frame = &pcm[i * HAPTIC_SYNTH_CHANNELS];
        frame[0] = 0;
        frame[1] = 0;
        for (size_t side = 0; side < 2; side++) {
            int32_t sample = 0;
            for (size_t band = 0; band < 2; band++) {
                sample += (sine_q15(state->phase[side][band]) * gain[side][band]) >> 15;
                state->phase[side][band] += step[side][band];
            }
            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }
            frame[2 + side] = (int16_t)sample;
        }
    }
}

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

/** 发声段音色的包络时长（48kHz 帧数）：起音 6ms、收音 14ms——段边界硬切
 *  满幅/零幅会在小喇叭上听成咔哒（查找手柄页刺耳声的来源之一）。 */
#define SYNTH_SPEAKER_ATTACK_FRAMES 288u
#define SYNTH_SPEAKER_RELEASE_FRAMES 672u
/** 发声段音色的二次谐波比例（Q15）：给蜂鸣一点中空腔体，接近 Joy-Con
 *  提示音的音色；谐波频率越过奈奎斯特界限就省去（混叠出不成调的杂音）。 */
#define SYNTH_SPEAKER_HARMONIC2 3604u /* 0.22 × 32768 */

/** 音圈包络门的起音/收音时长（48kHz 帧数）：起音 1ms 爬满、收音约 15ms 锁定最后发声的
 *  子帧淡出，避免块对齐硬切把短震动截没；与 PC 侧 ds5_haptics.py 同一条曲线。 */
#define SYNTH_COIL_ATTACK_FRAMES 48u  /* 1ms @ 48kHz */
#define SYNTH_COIL_RELEASE_FRAMES 728u
/* 包络的 Q15 步进（每帧，满幅 32768）：32768/48 与 32768/728。 */
#define SYNTH_COIL_RISE 683u
#define SYNTH_COIL_FALL 45u

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

/** 发声段音色的二次谐波合成峰值回缩（Q15）：基频+谐波最坏相位叠加约
 *  1.09 倍，压回峰值刻度。 */
#define SYNTH_SPEAKER_SHAPE 30048u /* 0.917 × 32768 */

/** 一个子帧槽位的振荡器组：门的开放侧跟序列当前子帧，收音尾侧跟锁定的
 *  子帧。开放侧按各侧自己声明的子帧数循环——主机是 200Hz 的单子帧流
 *  （实抓 94% 的包只声明 1 个子帧），声明之外的槽位不占时间；固定 3 槽
 *  轮播会把持续震动切成「5ms 有声 + 10ms 静默」的 66Hz 断续（音圈的细腻
 *  手感退化成普通马达的粗糙震动），合成的强震段同理。 */
static void osc_prepare_side(synth_osc_t *lf, synth_osc_t *hf, bool open,
                             const haptic_synth_state_t *state,
                             const haptic_synth_params_t *params, size_t side,
                             size_t key)
{
    uint16_t lf_freq = 0;
    uint8_t lf_gain = 0;
    uint16_t hf_freq = 0;
    uint8_t hf_gain = 0;
    const uint8_t keys = params->tones.key_count[side];
    if (open && keys != 0) {
        const size_t slot = key % keys;
        lf_freq = params->tones.key[side][slot].lf_freq;
        lf_gain = params->tones.key[side][slot].lf_gain;
        hf_freq = params->tones.key[side][slot].hf_freq;
        hf_gain = params->tones.key[side][slot].hf_gain;
    } else if (!open) {
        lf_freq = state->coil_latch[side].lf_freq;
        lf_gain = state->coil_latch[side].lf_gain;
        hf_freq = state->coil_latch[side].hf_freq;
        hf_gain = state->coil_latch[side].hf_gain;
    }
    *lf = osc_prepare(lf_freq, lf_gain, params->amp_peak);
    *hf = osc_prepare(hf_freq, hf_gain, params->amp_peak);
}

void haptic_synth_fill(haptic_synth_state_t *state, const haptic_synth_params_t *params,
                       int16_t *pcm, size_t frames)
{
    if (state == NULL || params == NULL || pcm == NULL) {
        return;
    }
    /* 发声段音色独立于 osc_prepare：包络的满幅带增益刻度（收音尾段增益
     * 已归零但包络还在落），相位步进跟频率走。 */
    const uint16_t speaker_freq = params->tones.speaker.freq;
    const uint32_t speaker_step = speaker_freq != 0 ? phase_step(speaker_freq) : 0;
    /* 包络满幅 = 增益刻度（Q16：65535 = 1.0，乘包络因此是 >> 16；gain 255 的
     * 目标 65536 差一位溢出，钳到 65535 只差万分之零点二）。 */
    const uint32_t speaker_target32 = params->tones.speaker.gain * 65536u / 255u;
    const uint16_t speaker_target =
        (uint16_t)(speaker_target32 > 65535u ? 65535u : speaker_target32);
    const int32_t speaker_harm =
        speaker_freq != 0 && (uint32_t)speaker_freq * 2u < HAPTIC_SYNTH_RATE_HZ / 2u
            ? (int32_t)SYNTH_SPEAKER_HARMONIC2
            : 0;
    const uint16_t slice = params->slice_frames != 0 ? params->slice_frames : 1;
    /* 子帧游标从上一块的断点继续：循环内逐帧倒数、到 0 切下一子帧并按新
     * 子帧重算振荡器步进（切帧不重置相位，时间轴上是连续波形）。 */
    /* 轮播长度取两侧声明子帧数的较大者（1..3）：声明 1 子帧的持续震动因此
     *  是连续的；两侧声明数不同时多的一侧照常轮播，少的一侧循环自己的序列。 */
    uint8_t cycle = params->tones.key_count[0];
    if (params->tones.key_count[1] > cycle) {
        cycle = params->tones.key_count[1];
    }
    if (cycle == 0 || cycle > PAD_HD_KEY_MAX) {
        cycle = PAD_HD_KEY_MAX;
    }
    uint8_t key = state->key_index;
    uint16_t left = state->slice_left;
    if (left == 0 || key >= cycle) {
        key = 0;
        left = slice;
    }
    /* 音圈包络门：每侧看子帧序列里有没有非零增益的子帧（第一个为准）——
     * 有 = 门开，并把它锁成收音尾的音色；没有 = 门关，收音尾按锁定子帧
     * 淡出，env 落到 0 清锁。两侧的门都关着（整段静默）时来了新震动：
     * 子帧时间轴与相位回零，新震动从自己的第一个子帧出去，不从上段震动
     * 的游标位置续播（子帧游标两侧共用，只在整段静默后重启）。 */
    bool coil_open[2];
    const bool coil_restart = state->coil_env[0] == 0 && state->coil_env[1] == 0;
    for (size_t side = 0; side < 2; side++) {
        const uint8_t keys = params->tones.key_count[side];
        coil_open[side] = false;
        for (size_t k = 0; k < PAD_HD_KEY_MAX && k < keys; k++) {
            if (params->tones.key[side][k].lf_gain != 0 ||
                params->tones.key[side][k].hf_gain != 0) {
                coil_open[side] = true;
                state->coil_latch[side].lf_freq = params->tones.key[side][k].lf_freq;
                state->coil_latch[side].lf_gain = params->tones.key[side][k].lf_gain;
                state->coil_latch[side].hf_freq = params->tones.key[side][k].hf_freq;
                state->coil_latch[side].hf_gain = params->tones.key[side][k].hf_gain;
                break;
            }
        }
        if (!coil_open[side] && state->coil_env[side] == 0) {
            memset(&state->coil_latch[side], 0, sizeof(state->coil_latch[side]));
        }
    }
    if (coil_restart && (coil_open[0] || coil_open[1])) {
        key = 0;
        left = slice;
        memset(state->phase, 0, sizeof(state->phase));
    }
    synth_osc_t lf[2];
    synth_osc_t hf[2];
    for (size_t side = 0; side < 2; side++) {
        osc_prepare_side(&lf[side], &hf[side], coil_open[side], state, params,
                         side, key);
    }
    for (size_t i = 0; i < frames; i++) {
        if (left == 0) {
            key = (uint8_t)((key + 1u) % cycle);
            left = slice;
            for (size_t side = 0; side < 2; side++) {
                osc_prepare_side(&lf[side], &hf[side], coil_open[side], state,
                                 params, side, key);
            }
        }
        left--;
        int16_t *frame = &pcm[i * HAPTIC_SYNTH_CHANNELS];
        int32_t speaker_sample = 0;
        if (speaker_target != 0 || state->speaker_env != 0) {
            /* 包络朝目标（有声=带增益刻度的满幅、无声=零）逐帧推进：段边界
             * 硬切满幅/零幅在小喇叭上听成咔哒，起音/收音各给一段过渡。 */
            if (speaker_target != 0) {
                const uint32_t env = (uint32_t)state->speaker_env + 228u;
                state->speaker_env =
                    (uint16_t)(env > speaker_target ? speaker_target : env);
            } else {
                state->speaker_env = state->speaker_env > 98u
                                         ? (uint16_t)(state->speaker_env - 98u)
                                         : 0u;
            }
            if (state->speaker_env != 0) {
                int32_t wave = sine_q15(state->speaker_phase);
                if (speaker_harm != 0) {
                    wave += (sine_q15(state->speaker_phase * 2u) * speaker_harm) >> 15;
                    wave = (wave * (int32_t)SYNTH_SPEAKER_SHAPE) >> 15;
                }
                speaker_sample = ((wave * params->amp_peak) >> 15) *
                                 (int32_t)state->speaker_env >> 16;
                state->speaker_phase += speaker_step;
            }
        }
        if (speaker_sample > 32767) {
            speaker_sample = 32767;
        } else if (speaker_sample < -32768) {
            speaker_sample = -32768;
        }
        frame[0] = (int16_t)speaker_sample;
        frame[1] = (int16_t)speaker_sample;
        for (size_t side = 0; side < 2; side++) {
            /* 包络门逐帧朝目标推进（门开=满幅、门关=收音淡出），波形乘在
             * 包络上；env 为 0 时连相位都不推进（长静默不空转）。 */
            if (coil_open[side]) {
                const uint32_t env = state->coil_env[side] + SYNTH_COIL_RISE;
                state->coil_env[side] = env > 32768u ? 32768u : (uint16_t)env;
            } else {
                state->coil_env[side] =
                    state->coil_env[side] > SYNTH_COIL_FALL
                        ? (uint16_t)(state->coil_env[side] - SYNTH_COIL_FALL)
                        : 0u;
            }
            int32_t sample = 0;
            if (state->coil_env[side] != 0) {
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
                sample = (sample * (int32_t)state->coil_env[side]) >> 15;
            }
            frame[2 + side] = (int16_t)sample;
        }
    }
    state->key_index = key;
    state->slice_left = left;
}

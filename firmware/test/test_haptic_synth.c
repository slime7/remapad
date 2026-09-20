/**
 * 触觉 PCM 合成（usb/haptic_synth.c）：NS2 的时序子帧经布局行 hd 规则重整
 * 后，在这里变成 DS5 音频通道上的 PCM——扬声器两路放采样提示音的发声段
 * （静音时恒零），触觉两路按各自子帧序列逐帧合成、按 slice_frames 帧数切
 * 子帧。钉住五件语义：无声音时扬声器两路恒零、左右触觉通道各跟各的子帧、
 * 子帧按时间顺序轮播且时间轴跨块连续、满幅钉在 amp_peak 刻度上、振荡器
 * 相位跨块连续（拼接处跳变会在音圈上听成咔哒声）。
 */
#include "host_test.h"

#include <string.h>

#include "haptic_synth.h"

static haptic_synth_params_t params_default(void)
{
    haptic_synth_params_t params;
    memset(&params, 0, sizeof(params));
    params.amp_peak = 24000;
    params.slice_frames = 10;
    for (size_t side = 0; side < 2; side++) {
        params.tones.key_count[side] = 3;
    }
    return params;
}

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

static void idle_params_produce_silence(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 1000; /* 盖过 48Hz 的整周期，切片切不到峰值的毛病别混进来 */

    int16_t pcm[300 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 300);
    for (size_t ch = 0; ch < HAPTIC_SYNTH_CHANNELS; ch++) {
        CHECK(channel_is_silent(pcm, 300, ch));
    }
}

/** 主机收震后的音圈收音尾：不是块对齐硬切——只占一块的短震动不被截没
 *  （收震后仍有声），收音在固定时长内平滑落回静音（结束及时，不拖长）。 */
static void coil_ringdown_is_bounded_after_host_stops(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 1200;
    params.tones.key[0][0].lf_freq = 48;
    params.tones.key[0][0].lf_gain = 255;

    int16_t pcm[800 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(peak_of(pcm, 400, 2) > 10000);

    params.tones.key[0][0].lf_gain = 0;
    haptic_synth_fill(&state, &params, pcm, 100);
    CHECK(peak_of(pcm, 100, 2) > 1000); /* 收震后第一块仍在收音尾 */
    haptic_synth_fill(&state, &params, pcm, 800); /* 100+800 帧盖过收音时长 */
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(channel_is_silent(pcm, 400, 2)); /* 收音时长过后落回静音 */
}

/** 整段静默后的新震动从子帧 0 起播：强子帧立刻出去，而不是从上一段震动
 *  停下的游标位置续播（那会让短震动的起拍落后最多两个子帧）。静默期的填充
 *  尺寸刻意不按切片对齐，让游标停在静默子帧的中段。 */
static void new_burst_after_silence_restarts_the_timeline(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 100;
    params.tones.key[0][0].lf_freq = 48; /* 子帧 0：强 */
    params.tones.key[0][0].lf_gain = 255;
    /* 子帧 1/2：静默 */

    int16_t pcm[450 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    haptic_synth_fill(&state, &params, pcm, 400);
    /* 静默两块（850 帧 > 收音时长，且不按切片对齐）：门落回零、游标停在
     * 静默子帧中段。 */
    params.tones.key[0][0].lf_gain = 0;
    haptic_synth_fill(&state, &params, pcm, 450);
    haptic_synth_fill(&state, &params, pcm, 400);
    /* 新震动：头 50 帧就是强子帧，不是游标所在的静默切片。 */
    params.tones.key[0][0].lf_gain = 255;
    haptic_synth_fill(&state, &params, pcm, 50);
    CHECK(peak_of(pcm, 50, 2) > 5000);
}

/** 没有真正的声音时扬声器两路填充 0 静音；触觉两路照常有波形。 */
static void speaker_channels_stay_silent_without_sound(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    for (size_t side = 0; side < 2; side++) {
        params.tones.key[side][0].lf_freq = 48;
        params.tones.key[side][0].lf_gain = 255;
        params.tones.key[side][0].hf_freq = 190;
        params.tones.key[side][0].hf_gain = 200;
    }

    int16_t pcm[400 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(channel_is_silent(pcm, 400, 0));
    CHECK(channel_is_silent(pcm, 400, 1));
    CHECK(peak_of(pcm, 400, 2) > 10000);
    CHECK(peak_of(pcm, 400, 3) > 10000);
}

/** 采样提示音的发声段铺到扬声器：两路同相的音色，触觉两路不受影响。 */
static void speaker_tone_rides_channels_1_2(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.tones.speaker.freq = 880;
    params.tones.speaker.gain = 255;

    int16_t pcm[400 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(peak_of(pcm, 400, 0) > 10000);
    CHECK(peak_of(pcm, 400, 1) > 10000);
    CHECK(channel_is_silent(pcm, 400, 2));
    CHECK(channel_is_silent(pcm, 400, 3));
    /* 两路是同一个小喇叭音色：逐样本相等。 */
    for (size_t i = 0; i < 400; i++) {
        CHECK_EQ(pcm[i * HAPTIC_SYNTH_CHANNELS + 0], pcm[i * HAPTIC_SYNTH_CHANNELS + 1]);
    }
}

/** 发声段的包络：起音渐入、收音渐出——段边界硬切满幅/零幅会在小喇叭上
 *  听成咔哒（查找手柄页刺耳声的来源之一）。起播第一帧远小于满幅、起音
 *  时长内爬到满幅；增益归零后按收音时长落回静音。 */
static void speaker_tone_fades_in_and_out(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 1200;
    params.tones.speaker.freq = 880;
    params.tones.speaker.gain = 255;

    int16_t pcm[720 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 480);
    CHECK(peak_of(pcm, 1, 0) < 4000);             /* 起音：第一帧不是满幅硬切 */
    CHECK(peak_of(pcm, 288, 0) > 18000);          /* 起音段内爬到满幅 */

    params.tones.speaker.gain = 0;
    haptic_synth_fill(&state, &params, pcm, 672 + 48);
    CHECK(peak_of(pcm, 54, 0) > 1000);            /* 收音起点附近还有声 */
    CHECK(channel_is_silent(pcm + 672 * HAPTIC_SYNTH_CHANNELS, 48, 0));
}

static void sides_follow_their_own_keys(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.tones.key[0][0].lf_freq = 48;
    params.tones.key[0][0].lf_gain = 255;

    int16_t pcm[1200 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1200);
    CHECK(peak_of(pcm, 1200, 2) > 20000);
    CHECK(channel_is_silent(pcm, 1200, 3));
}

/** 子帧按时间顺序轮播：slice_frames 帧一切，静默子帧原样保留在时间轴上
 *  （主机排好的节奏不能被压平成恒震）。 */
static void keys_play_in_order_over_the_timeline(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 100; /* 每切片足够长，低频音能走到峰值 */
    params.tones.key[0][0].lf_freq = 48;  /* 强 */
    params.tones.key[0][0].lf_gain = 255;
    /* 子帧 1：静默 */
    params.tones.key[0][2].lf_freq = 300; /* 弱 */
    params.tones.key[0][2].lf_gain = 64;

    int16_t pcm[400 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 400);
    CHECK(peak_of(pcm, 100, 2) > 10000);        /* 子帧 0：强 */
    CHECK(channel_is_silent(pcm + 100 * HAPTIC_SYNTH_CHANNELS, 100, 2)); /* 子帧 1：静默 */
    CHECK(peak_of(pcm + 200 * HAPTIC_SYNTH_CHANNELS, 100, 2) > 2000);    /* 子帧 2：弱 */
    CHECK(peak_of(pcm + 200 * HAPTIC_SYNTH_CHANNELS, 100, 2) < 8000);
    CHECK(peak_of(pcm + 300 * HAPTIC_SYNTH_CHANNELS, 100, 2) > 10000);   /* 回绕到子帧 0 */
}

static void full_amplitude_caps_at_scale(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 1200; /* 整段都在同一子帧上 */
    params.tones.key[0][0].lf_freq = 48; /* 一个周期 1000 样本，必扫过峰值。 */
    params.tones.key[0][0].lf_gain = 255;

    int16_t pcm[1200 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1200);
    const int peak = peak_of(pcm, 1200, 2);
    CHECK(peak >= 23500);
    CHECK(peak <= 24050);
}

/** amp_peak 是承载刻度：蓝牙私有流的 8-bit 形态用更小的峰值声明。 */
static void amp_peak_scales_the_render(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.amp_peak = 2400;
    params.slice_frames = 1200;
    params.tones.key[0][0].lf_freq = 48;
    params.tones.key[0][0].lf_gain = 255;

    int16_t pcm[1200 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 1200);
    const int peak = peak_of(pcm, 1200, 2);
    CHECK(peak >= 2300);
    CHECK(peak <= 2500);
}

/** 同一子帧内低频与高频叠加（NS2 每帧两条音），饱和封顶。 */
static void bands_sum_within_saturation(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 2400;
    params.tones.key[0][0].lf_freq = 48;
    params.tones.key[0][0].lf_gain = 255;
    params.tones.key[0][0].hf_freq = 190;
    params.tones.key[0][0].hf_gain = 128;

    int16_t pcm[2400 * HAPTIC_SYNTH_CHANNELS];
    haptic_synth_fill(&state, &params, pcm, 2400);
    CHECK(peak_of(pcm, 2400, 2) > 20000);
    CHECK(peak_of(pcm, 2400, 2) <= 32768);
    int min = 0;
    for (size_t i = 0; i < 2400; i++) {
        const int v = pcm[i * HAPTIC_SYNTH_CHANNELS + 2];
        if (v < min) {
            min = v;
        }
    }
    CHECK(min >= -32768);
    CHECK(min <= -23000);
}

/** 相位跨块连续，且跨子帧切换不重置相位：三个键同频同幅（幅度恒定，排除
 *  换键处合法的幅度阶跃），整条时间轴上相邻样本的最大跳变不超过同频正弦的
 *  斜率上界（24000×2π×48/48000 ≈ 151，留裕量到 400；重置相位会跳到
 *  ~48000）。 */
static void phase_stays_continuous_across_fills_and_keys(void)
{
    haptic_synth_state_t state;
    haptic_synth_reset(&state);
    haptic_synth_params_t params = params_default();
    params.slice_frames = 30; /* 频繁切子帧：块边界与帧边界交错 */
    params.tones.key_count[0] = 3;
    for (size_t k = 0; k < 3; k++) {
        params.tones.key[0][k].lf_freq = 48;
        params.tones.key[0][k].lf_gain = 255;
    }

    int16_t pcm[2000 * HAPTIC_SYNTH_CHANNELS];
    for (size_t off = 0; off < 2000; off += 1000) {
        haptic_synth_fill(&state, &params, pcm + off * HAPTIC_SYNTH_CHANNELS, 1000);
    }
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

static void band_freq_falls_back_and_clamps(void)
{
    CHECK_EQ(haptic_synth_band_freq(0, false), HAPTIC_SYNTH_FREQ_DEFAULT_LF);
    CHECK_EQ(haptic_synth_band_freq(0, true), HAPTIC_SYNTH_FREQ_DEFAULT_HF);
    CHECK_EQ(haptic_synth_band_freq(5, false), HAPTIC_SYNTH_FREQ_MIN);
    CHECK_EQ(haptic_synth_band_freq(600, true), HAPTIC_SYNTH_FREQ_MAX);
    CHECK_EQ(haptic_synth_band_freq(200, true), 200);
}

HOST_TEST_SUITE(suite_haptic_synth, "haptic_synth",
                {"静置参数输出纯零", idle_params_produce_silence},
                {"收震后音圈平滑收音且时长有界（短震动不被截没）",
                 coil_ringdown_is_bounded_after_host_stops},
                {"整段静默后的新震动从子帧 0 起播",
                 new_burst_after_silence_restarts_the_timeline},
                {"没有声音时扬声器恒零，触觉两路有波形",
                 speaker_channels_stay_silent_without_sound},
                {"发声段的小喇叭音色铺在频道 1/2", speaker_tone_rides_channels_1_2},
                {"发声段包络起音渐入收音渐出（段边界硬切听成咔哒）",
                 speaker_tone_fades_in_and_out},
                {"左右触觉通道各跟各的子帧", sides_follow_their_own_keys},
                {"子帧按时间顺序轮播，静默子帧保住节奏",
                 keys_play_in_order_over_the_timeline},
                {"满幅强度钉在刻度上界（24000 附近）", full_amplitude_caps_at_scale},
                {"amp_peak 缩放承载刻度（蓝牙 8-bit 形态）", amp_peak_scales_the_render},
                {"同一子帧内两带叠加且饱和封顶", bands_sum_within_saturation},
                {"相位跨块与跨子帧连续，切子帧不跳变",
                 phase_stays_continuous_across_fills_and_keys},
                {"频率字段零回落缺省、越界夹取", band_freq_falls_back_and_clamps});

"""ds5_haptics 的哑渲染：时序子帧表（固件按布局行重整）直接驱动振荡器，
采样字节本身不进合成；老固件的两带参数回落成每侧两条同时叠加的正弦。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ds5_haptics  # noqa: E402  （先把 pc/ 放进来再导入）


def hd_params() -> dict:
    return {"hd": {
        "l": {"count": 2, "keys": (((48, 255), (190, 64)), ((90, 2), (0, 0)))},
        "r": {"count": 1, "keys": (((0, 0), (484, 128)),)},
        "speaker": (880, 255),
    }}


class VoicesTest(unittest.TestCase):
    def test_sample_field_does_not_enter_synthesis(self):
        """触觉采样（0x0A 采样流）的原始 ID 只供日志展示：发声段由固件按
        音色表折成扬声器音色随 HD 段下发，PC 侧不消费采样字节。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"sample": 0x02, "lf_amp": (0, 0), "hf_amp": (0, 0)})
        hd = ds5_haptics._hd_voices(audio._params)
        self.assertIsNone(hd)

    def test_hd_keys_pass_through(self):
        """HD 子帧段原样进渲染：映射已在固件布局内完成，这里不做二次变换。"""
        params = hd_params()
        left, right, speaker = ds5_haptics._hd_voices(params)
        self.assertEqual(left["count"], 2)
        self.assertEqual(right["count"], 1)
        self.assertEqual(speaker, ((880, 255),))

    def test_legacy_bands_fall_back_to_two_tones(self):
        """老固件 16 字节帧：两带振幅与频率落地值回落成每侧两条同时叠加的
        正弦，扬声器恒零（老固件不向桥接渲染发声段）。"""
        params = {"lf_amp": (64, 32), "hf_amp": (16, 8),
                  "lf_freq": (55, 60), "hf_freq": (190, 200)}
        left, right, speaker = ds5_haptics._legacy_voices(params)
        self.assertEqual(left, ((55.0, 64), (190.0, 16)))
        self.assertEqual(right, ((60.0, 32), (200.0, 8)))
        self.assertEqual(speaker, ())


class RenderTest(unittest.TestCase):
    def test_silence_with_no_keys(self):
        """没有子帧就是纯零：主机停震后触觉流立即安静。"""
        side = {"count": 0, "keys": ()}
        out = ds5_haptics._render_keys(side, [0.0, 0.0], [0, 0], 32,
                                       ds5_haptics.BT_RATE,
                                       ds5_haptics.AMP_PEAK_USB,
                                       ds5_haptics._slice_samples(ds5_haptics.BT_RATE))
        self.assertEqual(out, [0] * 32)

    def test_key_produces_waveform_at_scale(self):
        """满幅子帧扫过峰值刻度，零增益子帧不出声。"""
        side = {"count": 3, "keys": (((55, 255), (0, 0)),) * 3}
        out = ds5_haptics._render_keys(side, [0.0, 0.0], [0, 0], 480,
                                       ds5_haptics.RATE, ds5_haptics.AMP_PEAK_USB,
                                       ds5_haptics._slice_samples(ds5_haptics.RATE))
        self.assertGreater(max(abs(v) for v in out), 20000)

        side = {"count": 3, "keys": (((55, 0), (0, 0)),) * 3}
        out = ds5_haptics._render_keys(side, [0.0, 0.0], [0, 0], 480,
                                       ds5_haptics.RATE, ds5_haptics.AMP_PEAK_USB,
                                       ds5_haptics._slice_samples(ds5_haptics.RATE))
        self.assertEqual(out, [0] * 480)

    def test_keys_play_in_time_order(self):
        """子帧按时间顺序轮播：强-静-弱的节奏在输出里按切片交替。"""
        side = {"count": 3, "keys": (((55, 255), (0, 0)),    # 子帧 0：强
                                     ((0, 0), (0, 0)),       # 子帧 1：静默
                                     ((55, 64), (0, 0)))}    # 子帧 2：弱
        out = ds5_haptics._render_keys(side, [0.0, 0.0], [0, 0], 45,
                                       ds5_haptics.BT_RATE, ds5_haptics.AMP_PEAK_USB,
                                       ds5_haptics._slice_samples(ds5_haptics.BT_RATE))
        # 3kHz 下每个切片 15 样本：0-14 强、15-29 静、30-44 弱。
        self.assertGreater(max(abs(v) for v in out[:15]), 6000)
        self.assertEqual(out[15:30], [0] * 15)
        self.assertGreater(max(abs(v) for v in out[30:]), 0)
        self.assertLess(max(abs(v) for v in out[30:]), max(abs(v) for v in out[:15]))

    def test_phase_carries_across_blocks(self):
        """相位跨块推进：两块拼接处不重置（相邻样本的跳变有正弦斜率上界，
        相位重置会跳到满刻度）。三个子帧同频同幅：切片边界不应产生任何跳变。"""
        side = {"count": 3, "keys": (((190, 200), (0, 0)),) * 3}
        phases = [0.0, 0.0]
        cursor = [0, 0]
        slice_samples = ds5_haptics._slice_samples(ds5_haptics.RATE)
        first = ds5_haptics._render_keys(side, phases, cursor, 96, ds5_haptics.RATE,
                                         ds5_haptics.AMP_PEAK_USB, slice_samples)
        second = ds5_haptics._render_keys(side, phases, cursor, 96, ds5_haptics.RATE,
                                          ds5_haptics.AMP_PEAK_USB, slice_samples)
        # 相邻样本差值上界 ≈ 18800×2π×190/48000 ≈ 463，留裕量到 2000。
        self.assertLessEqual(abs(second[0] - first[-1]), 2000)


class AudioCallbackTest(unittest.TestCase):
    def test_callback_block_layout(self):
        """WASAPI 回调的整块字节：4ch 交错 int16，扬声器两路放发声音色，
        触觉两路各跟各的子帧、发声段同时折进音圈（与蓝牙通路一致）。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params(hd_params())
        frames = 480
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch1 = [block[i * 4 + 0] for i in range(frames)]
        ch3 = [block[i * 4 + 2] for i in range(frames)]
        ch4 = [block[i * 4 + 3] for i in range(frames)]
        self.assertGreater(max(abs(v) for v in ch1), 10000)  # 发声段铺频道 1/2
        self.assertGreater(max(abs(v) for v in ch3), 10000)  # 左音圈
        # 右侧子帧 0 高频 484Hz：仅前 1/3 周期有声，其后音圈只剩折进来的
        # 发声段——与频道 1 的扬声器音色同值。
        self.assertGreater(max(abs(v) for v in ch4), 3000)
        self.assertEqual(ch4[240:], ch1[240:])

    def test_speaker_segment_reaches_coil_channels(self):
        """发声段折进两侧音圈：子帧全静、只有扬声器音色时，触觉两路与
        扬声器两路同样在响（蓝牙上没有扬声器通道，靠它保持行为一致）。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"hd": {
            "l": {"count": 0, "keys": ()},
            "r": {"count": 0, "keys": ()},
            "speaker": (500, 255)}})
        frames = 480
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch1 = [block[i * 4 + 0] for i in range(frames)]
        ch2 = [block[i * 4 + 1] for i in range(frames)]
        ch3 = [block[i * 4 + 2] for i in range(frames)]
        ch4 = [block[i * 4 + 3] for i in range(frames)]
        self.assertGreater(max(abs(v) for v in ch1), 10000)
        self.assertEqual(ch3, ch1)
        self.assertEqual(ch4, ch2)

    def test_legacy_callback_keeps_speaker_silent(self):
        """老固件两带参数：扬声器两路恒零。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"lf_amp": (255, 0), "hf_amp": (0, 0),
                          "lf_freq": (48, 48), "hf_freq": (190, 190)})
        frames = 480
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch1 = [block[i * 4 + 0] for i in range(frames)]
        ch3 = [block[i * 4 + 2] for i in range(frames)]
        self.assertEqual(max(abs(v) for v in ch1), 0)
        self.assertGreater(max(abs(v) for v in ch3), 10000)

    def test_speaker_tone_fades_in_and_out(self):
        """发声段带起音/收音包络：段边界硬切满幅/零幅会在小喇叭上听成咔哒
        （查找手柄页刺耳声的来源之一）——起播第一个样本远小于满幅，包络在
        起音时长内爬到满幅；增益归零后收音尾平滑落回静音。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"hd": {
            "l": {"count": 0, "keys": ()},
            "r": {"count": 0, "keys": ()},
            "speaker": (880, 255)}})
        frames = 960
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch1 = [block[i * 4 + 0] for i in range(frames)]
        self.assertLess(abs(ch1[0]), 4000)   # 起音：第一拍不是满幅硬切
        self.assertGreater(max(abs(v) for v in ch1), 18000)  # 包络内爬到满幅

        audio.set_params({"hd": {
            "l": {"count": 0, "keys": ()},
            "r": {"count": 0, "keys": ()},
            "speaker": (0, 0)}})
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch1 = [block[i * 4 + 0] for i in range(frames)]
        # 收音尾：起始处还有声，收音时长过后落回静音。
        self.assertGreater(abs(ch1[0]), 1000)
        release = round(ds5_haptics.SPEAKER_RELEASE_S * ds5_haptics.RATE)
        self.assertEqual(max(abs(v) for v in ch1[release + 1:]), 0)

    def test_coil_rumble_fades_after_host_stops(self):
        """主机收震后音圈走收音包络而不是块对齐硬切：收震后第一块立即还有声
        （短震动不被承载块边界截没）、收音时长内落回静音（结束及时）——
        与蓝牙 0x32 通路同一份门控行为。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        silent_side = {"count": 0, "keys": ()}
        audio.set_params({"hd": {
            "l": {"count": 1, "keys": (((55, 255), (0, 0)),)},
            "r": silent_side,
            "speaker": (0, 0)}})
        frames = 480
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        ch3 = [block[i * 4 + 2] for i in range(frames)]
        self.assertGreater(max(abs(v) for v in ch3), 10000)

        audio.set_params({"hd": {
            "l": silent_side,
            "r": silent_side,
            "speaker": (0, 0)}})
        tails = []
        for _ in range(3):
            tail = bytearray(frames * ds5_haptics.CHANNELS * 2)
            audio._callback(tail, frames, None, None)
            tails.append([memoryview(tail).cast("h")[i * 4 + 2]
                          for i in range(frames)])
        self.assertGreater(abs(tails[0][0]), 0)   # 收震后第一块仍在收音尾
        self.assertEqual(max(abs(v) for v in tails[2]), 0)  # 尾长过后全静


if __name__ == "__main__":
    unittest.main()

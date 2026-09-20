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
        触觉两路各跟各的子帧。"""
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
        # 右侧子帧 0 高频 484Hz：仅前 1/3 周期有声，其余切片静默。
        self.assertGreater(max(abs(v) for v in ch4), 3000)
        self.assertEqual(max(abs(v) for v in ch4[240:]), 0)

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


if __name__ == "__main__":
    unittest.main()

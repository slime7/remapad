"""ds5_haptics 的参数映射：FEEDBACK 采样字节（固件包络渲染出的幅度）进合成参数。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ds5_haptics  # noqa: E402  （先把 pc/ 放进来再导入）


class SetParamsTest(unittest.TestCase):
    def test_pulse_follows_envelope_amplitude(self):
        """固件把「搜索手柄」的持续采样渲染成节奏：PC 侧按幅度驱动，不是开关。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"sample": 0xC0, "lf_amp": (0, 0), "hf_amp": (0, 0)})
        self.assertEqual(audio._params["pulse"], 0xC0)
        audio.set_params({"sample": 0x80})
        self.assertEqual(audio._params["pulse"], 0x80)

    def test_pulse_zero_means_silence(self):
        """包络停顿段（sample 0）必须静默，不是恒定蜂鸣。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"sample": 0})
        self.assertEqual(audio._params["pulse"], 0)
        audio.set_params({})
        self.assertEqual(audio._params["pulse"], 0)


if __name__ == "__main__":
    unittest.main()

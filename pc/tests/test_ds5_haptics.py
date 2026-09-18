"""ds5_haptics 的参数映射：只吃 0x30 震动载波的两带，采样不进合成。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ds5_haptics  # noqa: E402  （先把 pc/ 放进来再导入）


class SetParamsTest(unittest.TestCase):
    def test_sample_field_does_not_enter_synthesis(self):
        """触觉采样（0x0A 采样流）是主机点播的声音：固件不再渲染给桥接路径，
        FEEDBACK 帧的采样字节只供日志展示——合成参数里不能出现采样派生项。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"sample": 0x02, "lf_amp": (0, 0), "hf_amp": (0, 0)})
        self.assertNotIn("pulse", audio._params)

    def test_band_amplitudes_pass_through(self):
        """两带振幅照常进参数，合成仍由 0x30 震动载波驱动。"""
        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"lf_amp": (64, 32), "hf_amp": (16, 8)})
        self.assertEqual(audio._params["lf_amp"], (64, 32))
        self.assertEqual(audio._params["hf_amp"], (16, 8))


if __name__ == "__main__":
    unittest.main()

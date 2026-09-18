"""FEEDBACK 帧载荷解析：两带强度、触觉采样与频率落地值（PC 侧音频触觉吃它）。

载荷布局与固件 input_link.c 一致：8 字节基础段，16 字节版本再带两带频率
（小端 u16 ×4）。老固件的 12 字节帧没有频率，解析按长度回退。
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）


def payload_with_freqs(lf_l=55, lf_r=60, hf_l=190, hf_r=200) -> bytes:
    return bytes([
        1, 1,            # 左右使能
        200, 128,        # 低频强度 L/R
        0x01, 0x02,      # 玩家灯 / 触觉采样
        96, 64,          # 高频强度 L/R
    ]) + (lf_l | lf_r << 16).to_bytes(4, "little") + (hf_l | hf_r << 16).to_bytes(4, "little")


class FeedbackParamsTest(unittest.TestCase):
    def test_full_payload_carries_bands_and_freqs(self):
        params = link.feedback_params(payload_with_freqs())
        self.assertEqual(params["rumble_on"], (True, True))
        self.assertEqual(params["lf_amp"], (200, 128))
        self.assertEqual(params["hf_amp"], (96, 64))
        self.assertEqual(params["player_led"], 0x01)
        self.assertEqual(params["sample"], 0x02)
        self.assertEqual(params["lf_freq"], (55, 60))
        self.assertEqual(params["hf_freq"], (190, 200))

    def test_legacy_payload_has_no_freqs(self):
        payload = payload_with_freqs()[:12]
        params = link.feedback_params(payload)
        self.assertEqual(params["lf_amp"], (200, 128))
        self.assertEqual(params["hf_amp"], (96, 64))
        self.assertIsNone(params["lf_freq"])
        self.assertIsNone(params["hf_freq"])

    def test_short_payload_is_rejected(self):
        self.assertIsNone(link.feedback_params(b"\x01" * 7))
        self.assertIsNone(link.feedback_params(b""))

    def test_round_trip_through_frame_decoder(self):
        payload = payload_with_freqs()
        frames, _text = link.FrameDecoder().feed(link.encode(link.TYPE_FEEDBACK, 0, 3, payload))
        params = link.feedback_params(frames[0][3])
        self.assertEqual(params["hf_freq"], (190, 200))


if __name__ == "__main__":
    unittest.main()

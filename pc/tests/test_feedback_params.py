"""FEEDBACK 帧载荷解析：两带强度、触觉采样、频率落地值与 HD 时序子帧表。

载荷布局与固件 pad_feedback_wire 一致：8 字节基础段，16 字节版本再带两带
频率（小端 u16 ×4），57 字节版本再带每侧 3 个时序子帧（低频/高频频率+增益）
与扬声器音色。老固件的 12 字节帧没有频率，解析按长度回退。
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


def payload_with_hd() -> bytes:
    """57 字节 HD 版：左 2 个有效子帧、右 1 个、扬声器发声。"""
    payload = bytearray(57)
    payload[:16] = payload_with_freqs()
    payload[16] = 2  # 左有效子帧数
    payload[17:23] = bytes([55, 0, 100, 190, 0, 64])    # 子帧 0：低 55Hz/100 + 高 190Hz/64
    payload[23:29] = bytes([90, 0, 2, 0, 0, 0])         # 子帧 1：低 90Hz/2，高频静默
    payload[35] = 1  # 右有效子帧数
    payload[36:42] = bytes([0xE4, 0x01, 128, 0, 0, 0])  # 子帧 0：低 484Hz/128，高频静默
    payload[54:56] = (880).to_bytes(2, "little")
    payload[56] = 255
    return bytes(payload)


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
        self.assertIsNone(params["hd"])

    def test_hd_payload_carries_temporal_keys(self):
        params = link.feedback_params(payload_with_hd())
        hd = params["hd"]
        self.assertEqual(hd["l"]["count"], 2)
        self.assertEqual(hd["l"]["keys"],
                         (((55, 100), (190, 64)), ((90, 2), (0, 0))))
        self.assertEqual(hd["r"]["count"], 1)
        self.assertEqual(hd["r"]["keys"], (((484, 128), (0, 0)),))
        self.assertEqual(hd["speaker"], (880, 255))
        # 基础段照常解析（打印与老通路共用一份参数）。
        self.assertEqual(params["lf_amp"], (200, 128))

    def test_hd_keys_beyond_count_are_dropped(self):
        payload = bytearray(payload_with_hd())
        payload[16] = 1  # 左侧只声明 1 个有效子帧
        params = link.feedback_params(bytes(payload))
        self.assertEqual(params["hd"]["l"]["keys"], (((55, 100), (190, 64)),))

    def test_legacy_payload_has_no_freqs(self):
        payload = payload_with_freqs()[:12]
        params = link.feedback_params(payload)
        self.assertEqual(params["lf_amp"], (200, 128))
        self.assertEqual(params["hf_amp"], (96, 64))
        self.assertIsNone(params["lf_freq"])
        self.assertIsNone(params["hf_freq"])
        self.assertIsNone(params["hd"])

    def test_short_payload_is_rejected(self):
        self.assertIsNone(link.feedback_params(b"\x01" * 7))
        self.assertIsNone(link.feedback_params(b""))

    def test_round_trip_through_frame_decoder(self):
        payload = payload_with_freqs()
        frames, _text = link.FrameDecoder().feed(link.encode(link.TYPE_FEEDBACK, 0, 3, payload))
        params = link.feedback_params(frames[0][3])
        self.assertEqual(params["hf_freq"], (190, 200))

    def test_hd_payload_round_trip_through_frame_decoder(self):
        payload = payload_with_hd()
        frames, _text = link.FrameDecoder().feed(link.encode(link.TYPE_FEEDBACK, 0, 3, payload))
        self.assertEqual(link.feedback_params(frames[0][3])["hd"]["speaker"], (880, 255))


if __name__ == "__main__":
    unittest.main()

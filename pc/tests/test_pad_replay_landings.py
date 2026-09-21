# -*- coding: utf-8 -*-
"""pad_replay.py 的落点行为：喇叭路由预置、0x31 的分带与感知曲线、私有流被拒
时的回落、以及连接方式按 bus_type 判定（0x0DF2 同时是有线 Edge 的 PID）。"""
import importlib.util
import io
import struct
import sys
import unittest
import zlib
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import ds5_haptics  # noqa: E402  （pc/ 已在 sys.path 上）

_SPEC = importlib.util.spec_from_file_location(
    "pad_replay", Path(__file__).resolve().parent / "samples" / "pad_replay.py")
pad_replay = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(pad_replay)


def key(lf_freq=0, lf_gain=0, hf_freq=0, hf_gain=0):
    return {"lf_freq": lf_freq, "lf_gain": lf_gain, "hf_freq": hf_freq, "hf_gain": hf_gain}


def side(**kwargs):
    return [key(**kwargs)] * 3


class FakeDevice:
    def __init__(self, fail_after=None):
        self.writes = []
        self.fail_after = fail_after
        self.closed = False

    def write(self, report):
        if self.fail_after is not None and len(self.writes) >= self.fail_after:
            raise OSError("写回被拒")
        self.writes.append(bytes(report))

    def close(self):
        self.closed = True


class FakeEncoder:
    def encode(self, pcm):
        return bytes(ds5_haptics.BT36_SPEAKER_BYTES)


class SimStub:
    """按固定内容回放的替身：keys 左右各一条子帧序列，speaker 给固定音色。"""

    def __init__(self, left=None, right=None, speaker=(0, 0)):
        self.left = left or side()
        self.right = right or side()
        self.speaker = speaker

    def count(self, _side):
        """主机声明的子帧数：桩固定按满 3 槽。"""
        return 3

    def at(self, _ms):
        return [self.left, self.right], self.speaker


class SpeakerRouteTest(unittest.TestCase):
    def test_setup_reports_carry_route_and_volume(self):
        """蓝牙与有线两份预置报告：输出路径位段 = 手柄喇叭（0x30）、前级 0x02、
        音量档 100、更新使能位都置上。不写这份报告时手柄内置喇叭未路由，
        0x36 的 Opus 喇叭块送进去也一声不出。"""
        bt = pad_replay.build_setup_0x31(0)
        self.assertEqual(len(bt), 78)
        self.assertEqual(bt[0], 0x31)
        self.assertEqual(bt[1], 0x00)
        self.assertEqual(bt[2], 0x10)
        self.assertEqual(bt[3], 0xA3)
        self.assertEqual(bt[4], 0x90)
        self.assertEqual(bt[8], 100)
        self.assertEqual(bt[10], 0x30)
        self.assertEqual(bt[40], 0x02)
        want = zlib.crc32(bt[:74], zlib.crc32(b"\xA2")) & 0xFFFFFFFF
        self.assertEqual(struct.unpack_from("<I", bt, 74)[0], want)

        usb = pad_replay.build_setup_0x02()
        self.assertEqual(len(usb), 48)
        self.assertEqual(usb[0], 0x02)
        self.assertEqual((usb[1], usb[2]), (0xA3, 0x90))
        self.assertEqual((usb[6], usb[8], usb[38]), (100, 0x30, 0x02))

    def test_bt36_landing_primes_the_route_before_the_stream(self):
        """0x36 落点先发一份 0x31 路由报告，再按节拍发 0x36；配置包的麦克风位
        保持清零（置位会把麦克风音频塞回输入报告，被当成摇杆满偏）。"""
        dev = FakeDevice()
        # 节拍按 interval×speed 推进 tick_ms，取三拍的窗口即可覆盖「先路由后流」。
        pad_replay.run_bt_36(dev, SimStub(speaker=(880, 255)), 450.0, 20.0,
                             FakeEncoder())
        self.assertGreaterEqual(len(dev.writes), 3)
        self.assertEqual(dev.writes[0][0], 0x31)
        self.assertEqual(dev.writes[0][10], 0x30)
        self.assertEqual(dev.writes[1][0], 0x36)
        self.assertEqual(dev.writes[1][4], 0xFE)
        self.assertEqual(dev.writes[2][0], 0x36)

    def test_private_stream_rejection_falls_back_to_hid(self):
        """私有流写回被拒（句柄失效/链路不接受）：提示并回落 0x31 HID 震动，
        整次回放不崩。"""
        fake_dev = FakeDevice()
        with mock.patch.object(pad_replay, "find_pad",
                               side_effect=lambda c: object() if c == "bt" else None), \
             mock.patch.object(pad_replay, "open_hid", return_value=fake_dev), \
             mock.patch.object(pad_replay, "run_bt_36",
                               side_effect=OSError("链路不接受")), \
             mock.patch.object(pad_replay, "run_bt_hid") as hid_run, \
             mock.patch.object(pad_replay.ds5_haptics, "Bt36OpusEncoder",
                               return_value=FakeEncoder()), \
             mock.patch.object(sys, "argv",
                               ["pad_replay.py", "ns2-search-page.capture",
                                "--pad", "bt36"]):
            out = io.StringIO()
            with redirect_stdout(out):
                pad_replay.main()
        self.assertIn("回落 0x31 HID 震动", out.getvalue())
        hid_run.assert_called_once()

    def test_find_pad_uses_bus_type_not_pid(self):
        """连接方式看 bus_type：0x0DF2 也是 DualSense Edge 的有线 PID，按 PID
        猜会把直插的 Edge 当成蓝牙。"""
        usb_edge = {"product_id": 0x0DF2, "bus_type": 1, "usage_page": 0x01,
                    "usage": 0x04, "path": b"usb-edge"}
        bt_ds5 = {"product_id": 0x0DF2, "bus_type": 2, "usage_page": 0x01,
                  "usage": 0x05, "path": b"bt-ds5"}
        fake_hid = mock.Mock()
        fake_hid.enumerate.return_value = [usb_edge, bt_ds5]
        with mock.patch.dict(sys.modules, {"hid": fake_hid}):
            self.assertIs(pad_replay.find_pad("usb"), usb_edge)
            self.assertIs(pad_replay.find_pad("bt"), bt_ds5)


class AutoLandingTest(unittest.TestCase):
    """--pad auto 跟产品通路一致：有线优先，蓝牙走私有触觉流（缺 Opus 退 0x32）"""

    def test_auto_prefers_usb_then_the_private_bt_stream(self):
        with mock.patch.object(pad_replay, "opus_available", return_value=True):
            self.assertEqual(pad_replay.resolve_pad("auto", object(), object()), "usb")
            self.assertEqual(pad_replay.resolve_pad("auto", None, object()), "bt36")

    def test_auto_degrades_without_opus_and_without_a_pad(self):
        with mock.patch.object(pad_replay, "opus_available", return_value=False):
            self.assertEqual(pad_replay.resolve_pad("auto", None, object()), "bt32")
        self.assertEqual(pad_replay.resolve_pad("auto", None, None), "bt")

    def test_explicit_choice_is_never_overridden(self):
        with mock.patch.object(pad_replay, "opus_available", return_value=True):
            for choice in ("usb", "bt", "bt32", "bt36", "bt39"):
                self.assertEqual(pad_replay.resolve_pad(choice, None, None), choice)

    def test_auto_run_lands_on_the_private_stream(self):
        """auto 在蓝牙上直接开私有流：缺 Opus 也走 0x32，而不是回 0x31"""
        dev = FakeDevice()
        with mock.patch.object(pad_replay, "find_pad",
                               side_effect=lambda c: object() if c == "bt" else None), \
             mock.patch.object(pad_replay, "open_hid", return_value=dev), \
             mock.patch.object(pad_replay, "opus_available", return_value=False), \
             mock.patch.object(pad_replay, "run_bt_32") as bt32_run, \
             mock.patch.object(pad_replay, "run_bt_hid") as hid_run, \
             mock.patch.object(sys, "argv",
                               ["pad_replay.py", "ns2-search-page.capture"]):
            out = io.StringIO()
            with redirect_stdout(out):
                pad_replay.main()
        self.assertIn("auto 落点：bt32", out.getvalue())
        bt32_run.assert_called_once()
        hid_run.assert_not_called()
        self.assertTrue(dev.closed)


class BtHidLandingTest(unittest.TestCase):
    def test_motors_follow_bands_with_the_perceived_curve(self):
        """0x31 落点与固件布局行等价：左大马达跟低频带、右小马达跟高频带，
        振幅过同一条感知曲线（小档位不在死区、直迁会几乎摸不到）。"""
        self.assertEqual(pad_replay.perceived_amp(0), 0)
        self.assertEqual(pad_replay.perceived_amp(255), 255)
        self.assertGreaterEqual(pad_replay.perceived_amp(1), 53)
        self.assertEqual(pad_replay.perceived_amp(64), 148)

        dev = FakeDevice()
        sim = SimStub(left=side(lf_gain=255), right=side(hf_gain=64))
        pad_replay.run_bt_hid(dev, sim, 10.0, 10.0)
        first = dev.writes[0]
        self.assertEqual(first[6], 255)   # 左大马达：低频带
        self.assertEqual(first[5], 148)   # 右小马达：高频带（过了感知曲线）
        self.assertEqual(first[10], 0x30)  # 预置一并带上：喇叭路由与音量档


class HdGainTest(unittest.TestCase):
    """触觉增益倍率（A/B 标定用）：只抬增益、夹回 8 位刻度，频率与段边界不动。"""

    @staticmethod
    def rumble_payload(lf_amp_10bit: int) -> bytes:
        """42 字节的 rumble 形态：占位字节 + 左右各 16 字节参数包。"""
        frame = (lf_amp_10bit << 10).to_bytes(5, "little")
        params = bytes([0x10]) + frame + bytes(10)  # 状态字声明 1 个有效子帧
        return bytes([0x00]) + params + params + bytes(9)

    def test_scaled_key_clips_at_the_scale_top(self):
        out = pad_replay.scaled_key(key(lf_freq=55, lf_gain=5, hf_freq=190, hf_gain=200),
                                    4.0)
        self.assertEqual((out["lf_gain"], out["hf_gain"]), (20, 255))
        self.assertEqual((out["lf_freq"], out["hf_freq"]), (55, 190))

    def test_sim_applies_the_gain_only_when_asked(self):
        payload = self.rumble_payload(lf_amp_10bit=20)  # HD 增益 = 20 >> 2 = 5
        plain, _ = pad_replay.FeedbackSim([(0.0, "rumble", payload)]).at(10.0)
        self.assertEqual(plain[0][0]["lf_gain"], 5)
        boosted, _ = pad_replay.FeedbackSim([(0.0, "rumble", payload)],
                                            hd_gain=4.0).at(10.0)
        self.assertEqual(boosted[0][0]["lf_gain"], 20)

    def test_sim_follows_the_declared_subframe_count(self):
        """回放与固件同一套轮播语义：轮播长度 = 主机声明的子帧数（声明之外的
        槽位不占时间）——实抓的游戏流每包只声明 1 个子帧，按固定 3 槽轮播会把
        持续震动切成 66Hz 断续。"""
        carrier = self.rumble_payload(lf_amp_10bit=20)  # 状态字 0x10 = 声明 1
        sim = pad_replay.FeedbackSim([(0.0, "rumble", carrier)])
        sim.at(10.0)
        self.assertEqual((sim.count(0), sim.count(1)), (1, 1))

        frame = (20 << 10).to_bytes(5, "little")
        params = bytes([0x30]) + frame + bytes(10)  # 状态字 0x30 = 声明 3
        sim = pad_replay.FeedbackSim([(0.0, "rumble", bytes([0x00]) + params + params + bytes(9))])
        sim.at(10.0)
        self.assertEqual((sim.count(0), sim.count(1)), (3, 3))


if __name__ == "__main__":
    unittest.main()

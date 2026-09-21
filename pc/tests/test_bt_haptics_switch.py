# -*- coding: utf-8 -*-
"""蓝牙私有触觉流的开关口径：DualSense 蓝牙接入默认走 0x36（HD 触觉 + 手柄
喇叭），要关得显式给 --no-bt-haptics（回落 0x31 两带震动）。"""
import unittest

import remapadctl


class BtHapticsSwitchTest(unittest.TestCase):
    def test_bt_private_stream_is_on_by_default(self):
        args = remapadctl.parse_args([])
        self.assertTrue(remapadctl.bt_haptics_wanted(args))

    def test_no_bt_haptics_opts_out(self):
        args = remapadctl.parse_args(["--no-bt-haptics"])
        self.assertFalse(remapadctl.bt_haptics_wanted(args))

    def test_explicit_on_flag_still_parses(self):
        args = remapadctl.parse_args(["--bt-haptics"])
        self.assertTrue(remapadctl.bt_haptics_wanted(args))


class HapticsYieldGateTest(unittest.TestCase):
    """让位（`haptic audio on`）等私有流真的接到 HD 子帧之后再发：没接到内容的
    通路不驱动音圈，提前让位会把手柄留在「HID 震动已清零、音频也没有内容」的
    静默状态（让位后没有震动、只剩玩家灯）。"""

    class FakeLink:
        def __init__(self) -> None:
            self.written: list[bytes] = []

        def write(self, data: bytes) -> None:
            self.written.append(data)

        def flush(self) -> None:
            pass

        def purge_input(self) -> None:
            pass

    class FakeHaptics:
        LABEL = "假触觉流"

        def __init__(self) -> None:
            self.engaged = False

        def stop(self) -> None:
            pass

    def _session(self):
        args = remapadctl.parse_args([])
        link = self.FakeLink()
        return remapadctl.Session(args, None, link), link

    def test_yield_waits_for_the_stream_to_engage(self):
        session, link = self._session()
        haptics = self.FakeHaptics()
        session.haptics = haptics
        session.pump_haptics_notify()
        self.assertNotIn(b"haptic audio on\r", link.written)
        haptics.engaged = True
        session.pump_haptics_notify()
        self.assertIn(b"haptic audio on\r", link.written)
        session.pump_haptics_notify()  # 已让位：不重复发
        self.assertEqual(link.written.count(b"haptic audio on\r"), 1)


if __name__ == "__main__":
    unittest.main()

"""反馈帧打印限频：震动效果包络里强度逐帧在变，逐条打印会把 GUI 日志区
（和命令行）刷爆——设备把等价帧判成变化时，稳态每秒上百条 `反馈 震动 … 强度 9/9`。
窗口内只放行第一条，其余合并计数，窗口结束时下一条带上合并数。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）


def feedback_payload(strength: int) -> bytes:
    """一条反馈帧载荷：左右使能 + 两带强度 + 玩家灯 + 触觉采样 + 高频带。"""
    return bytes([1, 1, strength, strength, 0x01, 0x00, strength, strength])


class FeedbackThrottleTest(unittest.TestCase):
    def test_first_frame_passes_and_burst_merges_with_count(self):
        gate = remapadctl.FeedbackThrottle(window_s=1.0)
        first = gate.feed(feedback_payload(15), now=100.0)
        self.assertIn("强度 15/15", first)
        self.assertNotIn("合并", first)

        # 同一窗口内的后续帧全部合并，只计数不产生输出。
        for i in range(120):
            self.assertIsNone(gate.feed(feedback_payload(9), now=100.0 + i * 0.005))

        # 窗口过后下一条放行，并带上合并掉的条数。
        merged = gate.feed(feedback_payload(9), now=102.0)
        self.assertIsNotNone(merged)
        self.assertIn("已合并 120 条", merged)
        self.assertIn("强度 9/9", merged)

    def test_merged_counter_resets_after_each_release(self):
        gate = remapadctl.FeedbackThrottle(window_s=1.0)
        gate.feed(feedback_payload(15), now=0.0)
        gate.feed(feedback_payload(9), now=0.1)
        gate.feed(feedback_payload(9), now=0.2)
        merged = gate.feed(feedback_payload(9), now=1.5)
        self.assertIn("已合并 2 条", merged)
        # 释放后计数归零：再过窗口来的新帧不带合并前缀。
        again = gate.feed(feedback_payload(9), now=3.0)
        self.assertNotIn("合并", again)

    def test_payload_format_includes_high_band(self):
        line = remapadctl.format_feedback(feedback_payload(9))
        self.assertIn("强度 9/9", line)
        self.assertIn("高频 9/9", line)


if __name__ == "__main__":
    unittest.main()

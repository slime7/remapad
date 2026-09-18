"""WriteBackGate：桥接写回限速——蓝牙 HID 写回慢，写回风暴把会话循环拖到输入转发卡顿。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from remapadctl import WriteBackGate  # noqa: E402  （先把 pc/ 放进来再导入）


class WriteBackGateTest(unittest.TestCase):
    def test_burst_is_capped_at_interval(self):
        """震动包络逐包都变、设备侧去重压不住：0.5 秒内涌进 50 帧只放行
        窗口数（约 17 帧 @30ms），会话循环不再被逐条写回拖住。"""
        gate = WriteBackGate(min_interval_s=0.03)
        writes = 0
        for i in range(50):
            if gate.admit(b"\x31" + bytes([i % 256]) + b"\x00" * 76, i * 0.01):
                writes += 1
        self.assertLessEqual(writes, 18)

    def test_pending_latest_lands_after_window(self):
        """被窗口挡下的帧不丢，留作最新待写帧：停震的最后一帧（马达全零）
        必须在窗口到期后落地，不然手柄会被钉在震动上。"""
        gate = WriteBackGate(min_interval_s=0.03)
        stop = b"\x31" + b"\x00" * 77
        self.assertTrue(gate.admit(b"\x31\x40" + b"\x00" * 76, 0.0))
        for t in (0.005, 0.01, 0.015, 0.02):
            self.assertFalse(gate.admit(b"\x31\x80" + b"\x00" * 76, t))
            self.assertFalse(gate.admit(stop, t + 0.001))
        # 窗口内不放行，到期后 poll 放行的是最新一帧（停震帧）。
        self.assertIsNone(gate.poll(0.02))
        self.assertEqual(gate.poll(0.05), stop)
        self.assertIsNone(gate.poll(0.05))


if __name__ == "__main__":
    unittest.main()

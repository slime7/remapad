"""设备信息行的拼装：界面这一行是设备屏幕「系统信息」页的镜像，口径要跟固件一致。

堆内存是唯一会对不齐的项：设备屏幕报的是内部堆的「已用 / 总量 KB」，
界面按空闲值显示时两个数字对不上。这里按固件 cli_status 的回读字段钉住拼装结果。
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadgui  # noqa: E402  （先把 pc/ 放进来再导入；导入不会建窗口）


class DeviceFactsLineTest(unittest.TestCase):
    def test_heap_uses_the_device_screen_used_over_total(self):
        text = remapadgui.format_device_facts({"heap": 88064, "heap_total": 327680})
        self.assertEqual(text, "堆内存 234 / 320 KB")

    def test_heap_without_total_falls_back_to_free(self):
        text = remapadgui.format_device_facts({"heap": 88064})
        self.assertEqual(text, "堆内存 空闲 86 KB")

    def test_full_line_keeps_the_existing_segments(self):
        text = remapadgui.format_device_facts({
            "firmware": "e04fd8c", "partition": "ota_0", "image": "confirmed",
            "ota_state": "idle", "battery_mv": 4160, "battery_percent": 100,
            "uptime_s": 9153, "pairing": "paired", "role": "device",
        })
        self.assertEqual(text.split("\n"), [
            "固件 e04fd8c ｜ 分区 ota_0 ｜ 镜像 已确认 ｜ 升级 idle",
            "电量 100% · 4.16V ｜ 运行 2:32:33 ｜ 配对 已配对 ｜ 角色 串口",
        ])

    def test_empty_facts_says_so(self):
        self.assertEqual(remapadgui.format_device_facts({}), "设备没有回可读的状态")


if __name__ == "__main__":
    unittest.main()

"""ViGEm 虚拟手柄排除：接口路径 → 设备实例 ID 的解析与祖先链判定。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）

EDGE_PATH = ("\\\\?\\HID#VID_054C&PID_0DF2&MI_03#8&2f3d4f&0&0000"
             "#{4d1e55b2-f16f-11cf-88cb-001111000030}")


class InstanceIdTest(unittest.TestCase):
    def test_parses_hid_interface_path(self):
        """hidapi 路径的前三段就是设备树实例 ID（# 换 \\）。"""
        self.assertEqual(remapadctl.instance_id_of_hid_path(EDGE_PATH),
                         "HID\\VID_054C&PID_0DF2&MI_03\\8&2f3d4f&0&0000")

    def test_rejects_non_interface_paths(self):
        self.assertIsNone(remapadctl.instance_id_of_hid_path(""))
        self.assertIsNone(remapadctl.instance_id_of_hid_path("COM3"))
        self.assertIsNone(remapadctl.instance_id_of_hid_path("\\\\?\\HID#only#one"))


class IsVirtualPadTest(unittest.TestCase):
    def test_unknown_paths_degrade_to_real(self):
        """设备树里查不到的路径按真实手柄处理，绝不静默丢设备。"""
        self.assertFalse(remapadctl.is_virtual_pad(EDGE_PATH))
        self.assertFalse(
            remapadctl.is_virtual_pad("\\\\?\\HID#VID_0000&PID_0000#bad&path#{0000}"))


class PartitionTest(unittest.TestCase):
    def test_splits_by_injected_predicate(self):
        real = {"path": "real"}
        fake = {"path": "fake"}
        real_list, virtual_list = remapadctl.partition_virtual(
            [real, fake], lambda p: p == "fake")
        self.assertEqual(real_list, [real])
        self.assertEqual(virtual_list, [fake])


if __name__ == "__main__":
    unittest.main()

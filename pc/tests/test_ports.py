"""串口枚举与打开失败的提示：link.py 里与设备无关的纯逻辑。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）


class SerialPortNamesTest(unittest.TestCase):
    def test_sorts_com_ports_by_number(self):
        values = [("\\Device\\USBSER000", "COM12"), ("\\Device\\BthModem0", "COM5"),
                  ("\\Device\\BthModem1", "COM6")]
        self.assertEqual(link.serial_port_names(values), ["COM5", "COM6", "COM12"])

    def test_drops_duplicates_and_foreign_names(self):
        values = [("a", "COM3"), ("b", "COM3"), ("c", "LPT1"), ("d", "BTH1")]
        self.assertEqual(link.serial_port_names(values), ["COM3"])

    def test_no_ports(self):
        self.assertEqual(link.serial_port_names([]), [])

    def test_unknown_port_names_sort_after_com_ports(self):
        self.assertEqual(link.port_sort_key("COM9"), ("", 9))
        self.assertEqual(link.port_sort_key("PRN1"), ("PRN1", 0))


class OpenHintTest(unittest.TestCase):
    def test_reports_busy_port(self):
        self.assertIn("占用", link.open_hint(OSError(32, "占用")))
        self.assertIn("占用", link.open_hint(OSError(5, "拒绝访问")))

    def test_reports_missing_port(self):
        self.assertIn("不存在", link.open_hint(OSError(2, "找不到")))

    def test_reports_other_failures(self):
        self.assertEqual(link.open_hint(OSError(1, "其他")), "打开端口失败")


if __name__ == "__main__":
    unittest.main()


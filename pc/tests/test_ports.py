"""串口枚举与打开失败的提示：link.py 里与设备无关的纯逻辑。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）
import remapadgui  # noqa: E402


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


class PortSelectionTest(unittest.TestCase):
    """界面默认选哪个口：落在错的口上会让人点「连接」时打不开或连错设备。"""

    def test_first_port_wins_before_the_user_picks_anything(self):
        self.assertEqual(remapadgui.port_selection(["COM5", "COM7"], "COM3", "COM3", False),
                         "COM5")

    def test_keeps_the_user_pick(self):
        self.assertEqual(remapadgui.port_selection(["COM5", "COM7"], "COM7", "COM3", True),
                         "COM7")

    def test_falls_back_when_nothing_is_plugged_in(self):
        self.assertEqual(remapadgui.port_selection([], "COM7", "COM3", True), "COM7")
        self.assertEqual(remapadgui.port_selection([], "", "COM3", False), "COM3")

    def test_leaves_a_cleared_box_empty(self):
        self.assertEqual(remapadgui.port_selection(["COM5"], "", "COM3", True), "")


class PortSummaryTest(unittest.TestCase):
    """界面里那行串口摘要：漏说「选中的口不在列表里」会让人以为设备还在。"""

    def test_marks_the_selected_port(self):
        self.assertEqual(remapadgui.port_summary(["COM3", "COM5"], "COM3"),
                         "串口 2 个：COM3（已选）  COM5")

    def test_says_when_the_selection_is_missing(self):
        self.assertIn("COM9 不在列表里", remapadgui.port_summary(["COM3"], "COM9"))

    def test_no_ports_points_at_the_refresh_button(self):
        text = remapadgui.port_summary([], "COM3")
        self.assertIn("没有检测到串口", text)
        self.assertIn("USB-Serial/JTAG", text)


if __name__ == "__main__":
    unittest.main()

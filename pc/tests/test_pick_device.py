"""手柄候选的挑选规则：用途过滤、VID/PID 过滤与按接口路径钉住。"""

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）


class FakeHid:
    def __init__(self, devices):
        self.devices = devices

    def enumerate(self):
        return self.devices


def device(path="if0", vid=0x054C, pid=0x0CE6, usage_page=0x01, usage=0x05, bus_type=1,
           product="Fake Pad"):
    return {
        "path": path,
        "vendor_id": vid,
        "product_id": pid,
        "usage_page": usage_page,
        "usage": usage,
        "product_string": product,
        "interface_number": 0,
        "bus_type": bus_type,
    }


def args(**overrides):
    base = {"vid": None, "pid": None, "pad_path": None}
    base.update(overrides)
    return SimpleNamespace(**base)


class PickDeviceTest(unittest.TestCase):
    def test_keeps_only_gamepad_usages(self):
        hid = FakeHid([device(path="keyboard", usage_page=0x06, usage=0x80),
                       device(path="pad", usage=0x04)])
        self.assertEqual(remapadctl.pick_device(args(), hid)["path"], "pad")

    def test_returns_none_without_candidates(self):
        self.assertIsNone(remapadctl.pick_device(args(), FakeHid([])))

    def test_filters_by_vendor_and_product(self):
        hid = FakeHid([device(path="ps"), device(path="xbox", vid=0x045E, pid=0x0B13)])
        self.assertEqual(remapadctl.pick_device(args(vid=0x045E), hid)["path"], "xbox")
        self.assertEqual(remapadctl.pick_device(args(vid=0x045E, pid=0x0B13), hid)["path"], "xbox")
        self.assertIsNone(remapadctl.pick_device(args(pid=0x1234), hid))

    def test_pad_path_pins_one_interface(self):
        hid = FakeHid([device(path="if0"), device(path="if1")])
        self.assertEqual(remapadctl.pick_device(args(), hid)["path"], "if0")
        self.assertEqual(remapadctl.pick_device(args(pad_path="if1"), hid)["path"], "if1")
        self.assertIsNone(remapadctl.pick_device(args(pad_path="if9"), hid))

    def test_describe_reports_family_and_bus(self):
        self.assertIn("ps", remapadctl.describe(device(bus_type=1)))
        self.assertIn("usb", remapadctl.describe(device(bus_type=1)))
        self.assertIn("bt", remapadctl.describe(device(vid=0x045E, bus_type=2)))


if __name__ == "__main__":
    unittest.main()


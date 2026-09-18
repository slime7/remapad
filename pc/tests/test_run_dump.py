"""--dump 的选柄规则：应与转发一致地按 --vid/--pid 过滤，而不是固定抓第一只。"""

import io
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）


class FakeDevice:
    def __init__(self):
        self.opened = None

    def open_path(self, path):
        self.opened = path

    def set_nonblocking(self, flag):
        pass

    def read(self, size):
        return []

    def close(self):
        pass


class FakeHid:
    def __init__(self, devices):
        self.devices = devices
        self.dev = FakeDevice()

    def enumerate(self):
        return self.devices

    def device(self):
        return self.dev


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
    base = {"vid": None, "pid": None, "pad_path": None, "seconds": 0.02}
    base.update(overrides)
    return SimpleNamespace(**base)


class RunDumpTest(unittest.TestCase):
    def test_opens_the_vid_pid_filtered_interface(self):
        hid = FakeHid([device(path="ps"), device(path="xbox", vid=0x045E, pid=0x0B13)])
        code = remapadctl.run_dump(args(vid=0x045E, pid=0x0B13), hid)
        self.assertEqual(code, 0)
        self.assertEqual(hid.dev.opened, "xbox")

    def test_keeps_first_interface_without_filter(self):
        hid = FakeHid([device(path="ps"), device(path="xbox", vid=0x045E, pid=0x0B13)])
        code = remapadctl.run_dump(args(), hid)
        self.assertEqual(code, 0)
        self.assertEqual(hid.dev.opened, "ps")

    def test_fails_with_hint_when_filter_matches_nothing(self):
        hid = FakeHid([device(path="ps")])
        with redirect_stderr(io.StringIO()) as err:
            code = remapadctl.run_dump(args(vid=0x1234), hid)
        self.assertEqual(code, 1)
        self.assertIn("--vid/--pid", err.getvalue())

    def test_prints_the_chosen_interface_in_dump_line(self):
        hid = FakeHid([device(path="ps"), device(path="xbox", vid=0x045E, pid=0x0B13,
                                                product="Xbox Pad")])
        with redirect_stdout(io.StringIO()) as out:
            code = remapadctl.run_dump(args(vid=0x045E), hid)
        self.assertEqual(code, 0)
        self.assertIn("Xbox Pad", out.getvalue())


if __name__ == "__main__":
    unittest.main()

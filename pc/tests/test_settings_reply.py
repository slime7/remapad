"""设置回读行的解析：界面控件跟着固件回读走，认错一行就会显示错的当前值。

样本是从固件 cli.c 的回读分支抄下来的真实格式（status / version / backlight /
screen / ctrl / ds），解析错了的表现是「界面显示的亮度、配色或开关与设备不一致」，
在 PC 上很难看出来，因此在这里逐条钉住。
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）
import remapadgui  # noqa: E402

# 固件 cli_status 的一行回读：pad 字段带空格（输入设备的型号描述）。
STATUS_LINE = ("state pairing=idle role=device backlight=60 screen=1 uptime=1234s "
               "heap=458752/524288 batt=3971mV/85% chg=0 fw=v0.4.0-12-gabcdef part=ota_0 "
               "ota=idle ui=off pad=DualSense Edge (USB)")


class DeviceReplyTest(unittest.TestCase):
    def test_backlight_reply_carries_percent(self):
        self.assertEqual(remapadctl.parse_device_reply("backlight 60"),
                         ("backlight", {"light": 60}))

    def test_backlight_reply_rejects_junk(self):
        for line in ("err backlight 0-100", "backlight 101", "backlight"):
            self.assertIsNone(remapadctl.parse_device_reply(line), line)

    def test_screen_reply_maps_on_and_off(self):
        self.assertEqual(remapadctl.parse_device_reply("screen on"),
                         ("screen", {"screen_on": True}))
        self.assertEqual(remapadctl.parse_device_reply("screen off"),
                         ("screen", {"screen_on": False}))
        self.assertIsNone(remapadctl.parse_device_reply("err usage: screen [on|off]"))

    def test_ctrl_reply_carries_the_four_color_segments(self):
        channel, fields = remapadctl.parse_device_reply(
            "ok ctrl body=0x1e3b2a button=0xc8a24a accent=0xc8a24a grip=0x16301f")
        self.assertEqual(channel, "ctrl")
        self.assertEqual(fields, {"body": 0x1E3B2A, "button": 0xC8A24A,
                                  "accent": 0xC8A24A, "grip": 0x16301F})

    def test_ctrl_reply_needs_all_four_segments(self):
        self.assertIsNone(remapadctl.parse_device_reply("ctrl body=0x232323"))
        self.assertIsNone(remapadctl.parse_device_reply("err colors are 0xRRGGBB"))

    def test_ds_reply_carries_both_switches(self):
        for line in ("ds touchpad=off capture=on",
                     "ds touchpad=off capture=on (persisted)"):
            self.assertEqual(remapadctl.parse_device_reply(line),
                             ("ds", {"touchpad_plus_minus": False, "capture_key": True}), line)
        self.assertIsNone(remapadctl.parse_device_reply("err usage: ds touchpad|capture [on|off]"))

    def test_status_reply_carries_device_facts(self):
        channel, fields = remapadctl.parse_device_reply(STATUS_LINE)
        self.assertEqual(channel, "device")
        self.assertEqual(fields["firmware"], "v0.4.0-12-gabcdef")
        self.assertEqual(fields["partition"], "ota_0")
        self.assertEqual(fields["ota_state"], "idle")
        self.assertEqual(fields["pairing"], "idle")
        self.assertEqual(fields["role"], "device")
        self.assertEqual(fields["light"], 60)
        self.assertTrue(fields["screen_on"])
        self.assertEqual(fields["uptime_s"], 1234)
        self.assertEqual(fields["heap"], 458752)
        self.assertEqual(fields["heap_total"], 524288)
        self.assertEqual((fields["battery_mv"], fields["battery_percent"]), (3971, 85))
        self.assertFalse(fields["charging"])
        # pad 字段的值里带空格（型号描述）：只保留第一段，但必须仍然存在。
        self.assertEqual(fields["pad"], "DualSense")

    def test_status_reply_from_an_older_firmware_keeps_free_only(self):
        channel, fields = remapadctl.parse_device_reply(
            "state pairing=idle role=device backlight=60 screen=1 uptime=1234s "
            "heap=458752 batt=3971mV/85% chg=0 fw=v0.4.0-12-gabcdef part=ota_0 ota=idle")
        self.assertEqual(channel, "device")
        self.assertEqual(fields["heap"], 458752)
        self.assertNotIn("heap_total", fields)

    def test_version_reply_carries_image_state(self):
        channel, fields = remapadctl.parse_device_reply(
            "fw=v0.4.0-12-gabcdef part=ota_1 image=pending-verify ota=idle")
        self.assertEqual(channel, "device")
        self.assertEqual(fields["partition"], "ota_1")
        self.assertEqual(fields["image"], "pending-verify")

    def test_other_lines_are_left_alone(self):
        for line in ("", "   ", "ok key injected", "ok mem report queued (printed next frame)",
                     "I (1234) remapad_cli: cli ready (type help)",
                     "pad ui mode on (dpad moves, circle confirms)"):
            self.assertIsNone(remapadctl.parse_device_reply(line), line)


class ColorInputTest(unittest.TestCase):
    """配色输入框的取值规则：写错不许发出去（错了会让设备报 err 或写错颜色）。"""

    def test_accepts_with_and_without_prefix(self):
        self.assertEqual(remapadgui.parse_color("0x232323"), 0x232323)
        self.assertEqual(remapadgui.parse_color(" 232323 "), 0x232323)
        self.assertEqual(remapadgui.parse_color("0X1E3B2A"), 0x1E3B2A)

    def test_rejects_junk(self):
        for text in ("", "0x", "1234567", "0xGGGGGG", "黑"):
            self.assertIsNone(remapadgui.parse_color(text), text)

    def test_swatch_colors_pick_readable_text(self):
        self.assertEqual(remapadgui.css_color(0x1E3B2A), "#1e3b2a")
        self.assertEqual(remapadgui.readable_on(0xB9BEC4), "#000000")   # 银灰是亮底
        self.assertEqual(remapadgui.readable_on(0x232323), "#ffffff")   # 标准黑是暗底

    def test_uptime_text_keeps_two_digit_fields(self):
        self.assertEqual(remapadgui.format_uptime(65), "01:05")
        self.assertEqual(remapadgui.format_uptime(3725), "1:02:05")


if __name__ == "__main__":
    unittest.main()

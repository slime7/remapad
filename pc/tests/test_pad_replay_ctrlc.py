# -*- coding: utf-8 -*-
"""pad_replay.py 的 Ctrl-C 收尾：回放中打断要干净退出（SystemExit 130），
音频流/HID 句柄的 finally 照常走到，不把 KeyboardInterrupt 裸抛给用户。"""
import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

_SPEC = importlib.util.spec_from_file_location(
    "pad_replay", Path(__file__).resolve().parent / "samples" / "pad_replay.py")
pad_replay = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(pad_replay)


class CtrlCExitTest(unittest.TestCase):
    def test_interrupted_replay_exits_cleanly(self):
        """回放循环里收到 KeyboardInterrupt：以 130 退出并打印收尾行。"""
        fake_dev = mock.Mock()
        with mock.patch.object(pad_replay, "find_pad", side_effect=lambda c: object() if c == "bt" else None), \
             mock.patch.object(pad_replay, "open_hid", return_value=fake_dev), \
             mock.patch.object(pad_replay, "run_bt_hid",
                               side_effect=KeyboardInterrupt), \
             mock.patch.object(sys, "argv", ["pad_replay.py",
                                             "ns2-search-page.capture",
                                             "--pad", "bt"]):
            out = io.StringIO()
            with redirect_stdout(out):
                with self.assertRaises(SystemExit) as ctx:
                    pad_replay.main()
        self.assertEqual(ctx.exception.code, 130)
        self.assertIn("已中断", out.getvalue())
        fake_dev.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()

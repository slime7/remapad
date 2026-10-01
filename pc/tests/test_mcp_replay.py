# -*- coding: utf-8 -*-
"""MCP 回放：TAS 式记录的解析与编译、回放线程的推进与打断、回放期的按键工具拒绝。"""

import sys
import threading
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ctrl  # noqa: E402  （先把 pc/ 放进来再导入）
import mcp_server  # noqa: E402
from mcp_server import McpError, compile_replay, parse_replay  # noqa: E402


class FakeLink:
    """够用的链路替身：只记录写出。"""

    def __init__(self):
        self.written: list[bytes] = []

    def write(self, data: bytes) -> None:
        self.written.append(data)

    def flush(self) -> None:
        pass


class FakeSession:
    """够用的会话替身：_session 守卫与 status 查询路径用。"""

    def __init__(self, conn):
        import queue
        self.link = conn
        self.seq = 0
        self.commands = queue.Queue()
        self.lines: list[str] = []

    def send_cli(self, command: str) -> None:
        self.lines.append(command)


def kinds(events) -> list[tuple]:
    return [(at_ms, kind, payload) for at_ms, _order, kind, payload in events]


class ParseReplayTest(unittest.TestCase):
    def test_default_frame_ms_and_segments(self):
        frame_ms, segments = parse_replay("|0|circle+cross|3000,2048|.\n|10|up|.|.|\n")
        self.assertAlmostEqual(frame_ms, 15.0)
        self.assertEqual(segments, [(0, ("circle", "cross"), (3000, 2048), None),
                                    (10, ("up",), None, None)])

    def test_frame_ms_and_fps_header_with_comments(self):
        frame_ms, segments = parse_replay("# 头部注释\r\nframe_ms = 2.5\r\n\r\n|0|circle|.|.|\r\n")
        self.assertEqual(frame_ms, 2.5)
        self.assertEqual(segments, [(0, ("circle",), None, None)])
        frame_ms, _segments = parse_replay("fps = 60\n|0|||\n")
        self.assertAlmostEqual(frame_ms, 1000.0 / 60.0)

    def test_empty_fields_mean_no_buttons_and_unchanged_sticks(self):
        _frame_ms, segments = parse_replay("|4||2048,2048|\n")
        self.assertEqual(segments, [(4, (), (2048, 2048), None)])
        _frame_ms, segments = parse_replay("|7|circle|.|.|")
        self.assertEqual(segments, [(7, ("circle",), None, None)])

    def test_rejects_bad_content(self):
        for bad in ("frame_ms = 0\n|0|||", "frame_ms = 5000\n|0|||", "wombat = 1\n|0|||",
                    "|3|circle|.|.\n|1|||", "|0|nope|.|.|", "|0|circle|9999,0|.|",
                    "|0|circle|1,2|x|", "|0|circle|1,2,3|.|", "-2\n|0|||", "hello\n|0|||",
                    "# 只有注释没有帧", ""):
            with self.assertRaises(McpError, msg=bad):
                parse_replay(bad)


class CompileReplayTest(unittest.TestCase):
    def test_buttons_become_down_up_and_sticks_sticky(self):
        events, span, frames = compile_replay(
            "frame_ms = 10\n|0|circle|4095,2048|.\n|5||.|.|\n|10|.|2048,2048|.\n", 1, 60000)
        self.assertEqual(kinds(events), [
            (0.0, "down", "circle"),
            (0.0, "stick", ("l", 4095, 2048)),
            (50.0, "up", "circle"),
            (100.0, "stick", ("l", 2048, 2048)),
        ])
        self.assertEqual(span, 110.0)
        self.assertEqual(frames, 11)

    def test_gap_holds_state_until_next_frame_line(self):
        events, span, frames = compile_replay("frame_ms = 10\n|0|circle|.|.\n|30||.|.\n", 1, 60000)
        self.assertEqual(kinds(events), [(0.0, "down", "circle"), (300.0, "up", "circle")])
        self.assertEqual(span, 310.0)
        self.assertEqual(frames, 31)

    def test_loop_boundary_releases_end_state(self):
        events, span, _frames = compile_replay("frame_ms = 10\n|0|circle|3000,3000|.\n|20||.|.\n", 3, 60000)
        self.assertEqual(kinds(events), [
            (0.0, "down", "circle"), (0.0, "stick", ("l", 3000, 3000)),
            (200.0, "up", "circle"),
            (210.0, "stick", ("l", 2048, 2048)),
        ])
        self.assertEqual(span, 210.0)

    def test_duration_cap_and_loop_range(self):
        text = "frame_ms = 10\n|0|circle|.|.\n|1||.|.\n"
        with self.assertRaises(McpError):
            compile_replay(text, 1, 10)
        for bad_loop in (0, -1, 1.5, True, 1001):
            with self.assertRaises(McpError, msg=bad_loop):
                compile_replay(text, bad_loop, 60000)
        compile_replay(text, 1000, 60000)  # 20s 总时长在上限内

    def test_event_cap(self):
        lines = "\n".join(f"|{i}|{'circle' if i % 2 else ''}|.|.|" for i in range(70000))
        with self.assertRaises(McpError):
            compile_replay(f"frame_ms = 1\n{lines}\n", 1, 600000)


class RunReplayTest(unittest.TestCase):
    def test_runs_timeline_and_clears_at_end(self):
        engine = mcp_server.KeyStateEngine()
        events, span, frames = compile_replay("frame_ms = 5\n|0|circle|.|.\n|2||.|.\n", 1, 1000)
        progress = mcp_server.ReplayProgress("x.tas", 1, span, frames)
        duration = mcp_server.run_replay(engine, events, 1, span, threading.Event(), progress)
        self.assertGreaterEqual(duration, span)
        self.assertEqual(engine.state()["held"], [])
        snap = progress.snapshot()
        self.assertEqual(snap["frames"], 3)
        self.assertGreaterEqual(snap["elapsed_ms"], span)

    def test_held_key_visible_mid_run_and_stop_interrupts(self):
        engine = mcp_server.KeyStateEngine()
        events, span, frames = compile_replay("frame_ms = 10\n|0|circle|.|.\n|49||.|.\n", 1, 60000)
        progress = mcp_server.ReplayProgress("x.tas", 1, span, frames)
        stop_event = threading.Event()
        worker = threading.Thread(target=mcp_server.run_replay,
                                  args=(engine, events, 1, span, stop_event, progress), daemon=True)
        worker.start()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and engine.state()["held"] != ["circle"]:
            time.sleep(0.005)
        self.assertEqual(engine.state()["held"], ["circle"])
        stop_event.set()
        worker.join(timeout=2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(engine.state()["held"], [])
        self.assertLess(progress.snapshot()["elapsed_ms"], span)


class ReplayBridgeTest(unittest.TestCase):
    """桥接面：回放槽互斥、结束/打断收尾、按键工具在回放期的拒绝与状态回报。"""

    def setUp(self):
        self.bridge = mcp_server.PadBridge(ctrl.parse_args(["--no-pad"]), 60000, 600000,
                                           ping_period_s=1.0)
        self.bridge._session = lambda: FakeSession(FakeLink())
        self._dir = TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)

    def write(self, text: str) -> str:
        path = str(Path(self._dir.name) / "movie.tas")
        Path(path).write_text(text, encoding="utf-8")
        return path

    def wait_active(self, timeout: float = 2.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snap = self.bridge.replay_snapshot()
            if snap["active"]:
                return snap
            time.sleep(0.005)
        self.fail("回放没有进入活跃状态")

    def wait_done(self, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snap = self.bridge.replay_snapshot()
            if not snap["active"]:
                return snap
            time.sleep(0.005)
        self.fail("回放没有在期限内结束")

    def test_replay_lifecycle_runs_and_allows_restart(self):
        path = self.write("frame_ms = 5\n|0|circle|.|.\n|2||.|.\n")
        plan = self.bridge.start_replay(path, 1)
        self.assertTrue(plan["ok"])
        self.assertEqual(plan["frames"], 3)
        self.assertEqual(plan["span_ms"], 15)
        snap = self.wait_done()
        self.assertTrue(snap["interrupted"] is False)
        self.assertEqual(self.bridge.engine.state()["held"], [])
        self.assertTrue(self.bridge.start_replay(path, 1)["ok"])  # 结束后可再次启动
        self.bridge.stop_replay()
        self.wait_done()

    def test_start_requires_connection_and_slot_is_exclusive(self):
        path = self.write("frame_ms = 10\n|0|circle|.|.\n|49||.|.\n")
        strict = mcp_server.PadBridge(ctrl.parse_args(["--no-pad"]), 60000, 600000, ping_period_s=1.0)
        with self.assertRaises(McpError):
            strict.start_replay(path, 1)
        self.assertTrue(self.bridge.start_replay(path, 1)["ok"])
        self.wait_active()
        with self.assertRaises(McpError):
            self.bridge.start_replay(path, 1)
        self.bridge.stop_replay()
        self.wait_done()

    def test_stop_interrupts_and_is_idempotent(self):
        path = self.write("frame_ms = 10\n|0|circle|.|.\n|49||.|.\n")
        self.bridge.start_replay(path, 1)
        self.wait_active()
        result = self.bridge.stop_replay()
        self.assertTrue(result["stopped"])
        snap = self.wait_done()
        self.assertTrue(snap["interrupted"])
        self.assertEqual(self.bridge.engine.state()["held"], [])
        self.assertFalse(self.bridge.stop_replay()["stopped"])  # 无回放时幂等成功

    def test_disconnect_aborts_running_replay(self):
        path = self.write("frame_ms = 10\n|0|circle|.|.\n|49||.|.\n")
        self.bridge.start_replay(path, 1)
        self.wait_active()
        self.assertTrue(self.bridge.disconnect()["ok"])
        snap = self.wait_done()
        self.assertTrue(snap["interrupted"])
        self.assertEqual(self.bridge.engine.state()["held"], [])

    def test_rejects_unreadable_and_oversize_files(self):
        with self.assertRaises(McpError):
            self.bridge.start_replay(str(Path(self._dir.name) / "missing.tas"), 1)
        big = Path(self._dir.name) / "big.tas"
        big.write_bytes(b"|0|||\n" * (mcp_server.MAX_REPLAY_BYTES // 6 + 1))
        with self.assertRaises(McpError):
            self.bridge.start_replay(str(big), 1)


class ReplayToolSurfaceTest(unittest.TestCase):
    """工具面：回放进行中按键类工具统一拒绝，status 与错误信息明确报出回放任务。"""

    def setUp(self):
        self.bridge = mcp_server.PadBridge(ctrl.parse_args(["--no-pad"]), 60000, 600000,
                                           ping_period_s=1.0)
        self.bridge._session = lambda: FakeSession(FakeLink())
        self.bridge.ctrl_args.reply_wait = 0.01
        self._old_bridge = mcp_server.BRIDGE
        mcp_server.BRIDGE = self.bridge
        self.addCleanup(setattr, mcp_server, "BRIDGE", self._old_bridge)
        self._dir = TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)
        path = str(Path(self._dir.name) / "movie.tas")
        Path(path).write_text("frame_ms = 10\n|0|circle|.|.\n|49||.|.\n", encoding="utf-8")
        self.assertTrue(mcp_server.remapad_replay(path=path)["ok"])
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not self.bridge.replay_snapshot()["active"]:
            time.sleep(0.005)

    def tearDown(self):
        self.bridge.stop_replay()

    def test_key_tools_refuse_with_replay_hint(self):
        self.assertIn("remapad_replay_stop", mcp_server.remapad_tap(["circle"])["error"])
        self.assertIn("回放", mcp_server.remapad_hold(["circle"])["error"])
        self.assertIn("回放", mcp_server.remapad_stick("l", 0, 0)["error"])
        self.assertIn("回放", mcp_server.remapad_stick_reset()["error"])
        self.assertIn("回放", mcp_server.remapad_script([{"t": 0, "tap": ["circle"]}])["error"])
        self.assertIn("回放", mcp_server.remapad_release_all()["error"])
        self.assertIn("回放", mcp_server.remapad_replay(path="another.tas")["error"])

    def test_status_reports_active_replay(self):
        status = mcp_server.remapad_status()
        self.assertTrue(status["ok"])
        self.assertTrue(status["replay"]["active"])
        self.assertLess(status["replay"]["frame"], status["replay"]["frames"])
        self.assertEqual(status["replay"]["loop"], 1)


if __name__ == "__main__":
    unittest.main()

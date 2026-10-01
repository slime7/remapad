"""MCP 按键服务纯逻辑：键名表、CLI 注入引擎的命令序列、脚本时间线与工具注册面。"""

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）
import ctrl  # noqa: E402
import mcp_server  # noqa: E402
from mcp_server import (  # noqa: E402
    KEY_NAMES,
    MAX_KEYS_PER_CALL,
    McpError,
    compile_script,
    validate_keys,
)


class FakeLink:
    """记录写出的帧字节，供 PING 保活断言。"""

    def __init__(self):
        self.written: list[bytes] = []

    def write(self, data: bytes) -> None:
        self.written.append(data)

    def flush(self) -> None:
        pass


class FakeUdpLink(link.UdpLink):
    """借 UdpLink 的类型身份触发引擎的 UDP 保活分支，不真开套接字。"""

    def __init__(self) -> None:
        self.written: list[bytes] = []

    def write(self, data: bytes) -> None:
        self.written.append(data)

    def flush(self) -> None:
        pass


class FakeSession:
    """够用的会话替身：send_cli 记录文本行，link 承载帧写。"""

    def __init__(self, conn):
        self.link = conn
        self.seq = 0
        self.lines: list[str] = []

    def send_cli(self, command: str) -> None:
        self.lines.append(command)


class KeyNamesTest(unittest.TestCase):
    def test_key_names_match_firmware_cli_table(self):
        expected = {"circle", "cross", "triangle", "square", "opt", "touchpad", "home", "share",
                    "mute", "l1", "r1", "l4", "r4", "l3", "r3", "up", "down", "left", "right"}
        self.assertEqual(set(KEY_NAMES), expected)

    def test_analog_trigger_and_ui_combo_not_exposed(self):
        for bad in ("l2", "r2", "ui", "a", "zl", "gl"):
            with self.assertRaises(McpError, msg=bad):
                validate_keys([bad])


class ValidateKeysTest(unittest.TestCase):
    def test_normalizes_case_and_dedupes_keeps_order(self):
        self.assertEqual(validate_keys(["Circle", "circle", " R1 "]), ["circle", "r1"])

    def test_rejects_unknown_and_empty_and_too_many(self):
        with self.assertRaises(McpError):
            validate_keys(["nope"])
        with self.assertRaises(McpError):
            validate_keys([])
        with self.assertRaises(McpError):
            validate_keys([str(i) for i in range(MAX_KEYS_PER_CALL + 1)])
        self.assertEqual(validate_keys([], allow_empty=True), [])


class EngineTest(unittest.TestCase):
    """引擎时钟由 tick 维护：先打一拍设定时钟基，再变更状态、再 flush。"""

    def lines(self, session) -> list[str]:
        result, session.lines = session.lines, []
        return result

    def test_tap_enters_state_and_sends_finite_command(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.tap(["circle"], 200)
        engine.tick(100.0, session)
        self.assertEqual(self.lines(session), ["key circle 200"])

    def test_tap_expiry_prunes_without_release_command(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.tap(["circle"], 200)
        engine.tick(100.0, session)
        self.lines(session)
        engine.tick(100.25, session)  # 到期剪枝：固件已自行清掉掩码
        self.assertEqual(self.lines(session), [])
        self.assertEqual(engine.state()["held"], [])

    def test_press_sends_rolling_hold(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.0, session)
        self.assertEqual(self.lines(session), ["key circle 55000"])

    def test_second_press_resends_descending_remaining(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(101.0, session)
        self.assertEqual(self.lines(session), ["key circle 54000"])
        engine.press(["cross"])
        engine.tick(101.0, session)
        self.assertEqual(self.lines(session), ["key cross 55000", "key circle 54000"])

    def test_finite_tap_joins_state_when_holding(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.0, session)
        self.lines(session)
        engine.tap(["triangle"], 200)
        engine.tick(100.05, session)
        # 剩余降序：circle 在前，triangle 的 150ms 是最后一条（=最早期限）。
        self.assertEqual(self.lines(session), ["key circle 54950", "key triangle 150"])

    def test_finite_expiry_with_survivor_resends_it(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.0, session)
        engine.tap(["triangle"], 200)
        engine.tick(100.05, session)
        self.lines(session)
        engine.tick(100.3, session)  # triangle 到期：固件清了整个掩码，存活键需立刻重发
        self.assertEqual(self.lines(session), ["key circle 54700"])

    def test_subset_release_sends_release_then_resend(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle", "cross"])
        engine.tick(100.0, session)
        self.lines(session)
        engine.release(["circle"])
        engine.tick(100.05, session)
        self.assertEqual(self.lines(session), ["key release", "key cross 54950"])

    def test_release_last_key_sends_only_release(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.0, session)
        self.lines(session)
        engine.release(["circle"])
        engine.tick(100.05, session)
        self.assertEqual(self.lines(session), ["key release"])
        self.assertEqual(engine.state()["held"], [])

    def test_rolling_hold_refreshes_before_expiry(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.0, session)
        self.lines(session)
        engine.tick(154.6, session)  # 余额不足 5s：滚动续期重发
        self.assertEqual(self.lines(session), ["key circle 55000"])

    def test_stick_and_reset_and_clear(self):
        engine = mcp_server.KeyStateEngine()
        session = FakeSession(FakeLink())
        engine.tick(100.0, session)
        engine.set_stick("l", 4095, 2048)
        engine.tick(100.05, session)
        self.assertEqual(self.lines(session), ["stick l 4095 2048"])
        engine.press(["circle"])
        engine.tick(100.1, session)
        self.lines(session)
        engine.clear()
        engine.tick(100.15, session)
        self.assertEqual(self.lines(session), ["key release", "stick reset"])
        self.assertEqual(engine.state()["held"], [])

    def test_udp_link_gets_ping_keepalive(self):
        engine = mcp_server.KeyStateEngine(ping_period_s=0.1)
        conn = FakeUdpLink()
        session = FakeSession(conn)
        engine.tick(100.0, session)
        engine.press(["circle"])
        engine.tick(100.05, session)
        engine.tick(100.11, session)
        decoder = link.FrameDecoder()
        pings = 0
        for blob in conn.written:
            got, _text = decoder.feed(blob)
            pings += sum(1 for frame_type, _slot, _seq, _payload in got if frame_type == link.TYPE_PING)
        self.assertEqual(pings, 2)


class CompileScriptTest(unittest.TestCase):
    def test_tap_expands_to_down_and_up(self):
        events, span = compile_script([{"t": 10, "tap": ["circle"], "hold_ms": 100}], 1, 60000)
        kinds = [(at_ms, kind, payload) for at_ms, _order, kind, payload in events]
        self.assertEqual(kinds, [(10.0, "down", "circle"), (110.0, "up", "circle")])
        self.assertEqual(span, 110.0)

    def test_same_time_runs_up_before_down(self):
        events, _span = compile_script([{"t": 0, "up": ["circle"], "down": ["cross"]}], 1, 60000)
        self.assertEqual([kind for _t, _o, kind, _p in events], ["up", "down"])

    def test_stick_and_reset_compile(self):
        events, _span = compile_script(
            [{"t": 0, "stick": {"side": "l", "x": 4095, "y": 2048}}, {"t": 50, "stick_reset": True}], 1, 60000)
        payloads = [payload for _t, _o, kind, payload in events if kind == "stick"]
        self.assertEqual(payloads, [("l", 4095, 2048)])

    def test_total_duration_cap(self):
        with self.assertRaises(McpError):
            compile_script([{"t": 1000, "tap": ["circle"]}], 100, 60000)

    def test_rejects_bad_input(self):
        with self.assertRaises(McpError):
            compile_script([], 1, 60000)
        with self.assertRaises(McpError):
            compile_script([{"t": 0, "down": ["nope"]}], 1, 60000)
        with self.assertRaises(McpError):
            compile_script([{"t": 0, "tap": ["circle"]}], 0, 60000)
        with self.assertRaises(McpError):
            compile_script([{"t": 0, "stick": {"side": "x", "x": 0, "y": 0}}], 1, 60000)


class ReplySinkTest(unittest.TestCase):
    def test_noise_lines_are_filtered(self):
        sink = mcp_server.ReplySink()
        for text in ("设备在线（协议 v1）", "已转发 0 帧报告", "反馈 震动 L=off R=off",
                     "ok key injected", "ok keys released", "ok stick set", "ok sticks centered"):
            sink.line(text)
        self.assertEqual(sink.collect(sink.mark(), 0.01), [])

    def test_secrets_masked_and_lines_collected(self):
        sink = mcp_server.ReplySink()
        mark = sink.mark()
        sink.line("ok netlog cred ssid=slime_nest pass=hunter2")
        sink.line("ok key injected")  # 注入应答属噪音，不入收集
        lines = sink.collect(mark, 0.01)
        self.assertEqual(lines, ["ok netlog cred ssid=slime_nest pass=***"])
        self.assertEqual(sink.collect(sink.mark(), 0.01), [])  # 收取即消费


class BridgeGuardTest(unittest.TestCase):
    """连接守卫：未建链时设备类工具报错而不是隐式开串口。"""

    def test_device_tools_refuse_before_connect(self):
        bridge = mcp_server.PadBridge(ctrl.parse_args(["--no-pad"]), 60000, 1.0)
        with self.assertRaises(mcp_server.McpError):
            bridge.query("status")
        with self.assertRaises(mcp_server.McpError):
            bridge.screenshot(None)
        with self.assertRaises(mcp_server.McpError):
            bridge.connect(None, None)
        with self.assertRaises(mcp_server.McpError):
            bridge.connect("COM12", "192.168.1.5")
        self.assertEqual(bridge.disconnect()["ok"], True)  # 未连接时断开是幂等成功


class ToolSurfaceTest(unittest.TestCase):
    def test_tools_registered_with_typed_schemas(self):
        tools = {tool.name: tool for tool in asyncio.run(mcp_server.SERVER.list_tools())}
        expected = {
            "remapad_connect", "remapad_disconnect", "remapad_status", "remapad_pair", "remapad_drop",
            "remapad_tap", "remapad_hold", "remapad_stick", "remapad_stick_reset", "remapad_script",
            "remapad_release_all", "remapad_screenshot",
        }
        self.assertEqual(set(tools), expected)
        tap_schema = tools["remapad_tap"].input_schema
        self.assertEqual(set(tap_schema["properties"]), {"keys", "hold_ms"})
        self.assertEqual(tap_schema["properties"]["keys"]["type"], "array")
        script_schema = tools["remapad_script"].input_schema
        self.assertEqual(set(script_schema["properties"]), {"actions", "loop"})
        connect_schema = tools["remapad_connect"].input_schema
        self.assertEqual(set(connect_schema["properties"]), {"port", "net"})
        self.assertEqual(set(tools["remapad_disconnect"].input_schema.get("properties", {})), set())


if __name__ == "__main__":
    unittest.main()

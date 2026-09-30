"""会话输出的分流：命令行写标准流、界面写队列，以及命令行的本行命令解析。"""

import contextlib
import io
import queue
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）
import remapadctl  # noqa: E402
import gui  # noqa: E402  （界面侧的队列接收器在这里，导入不会建窗口）


class FakeLink:
    """够用的串口替身：只记录写出去的字节。"""

    def __init__(self):
        self.written: list[bytes] = []
        self.flushed = 0

    def write(self, data: bytes) -> None:
        self.written.append(data)

    def flush(self) -> None:
        self.flushed += 1

    def purge_input(self) -> None:
        pass


def session(reporter=None):
    args = remapadctl.parse_args([])
    fake = FakeLink()
    return remapadctl.Session(args, None, fake, reporter=reporter), fake


class ConsoleReporterTest(unittest.TestCase):
    def test_line_goes_to_stdout_and_error_to_stderr(self):
        out, err = io.StringIO(), io.StringIO()
        reporter = remapadctl.ConsoleReporter()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            reporter.line("普通行")
            reporter.error("错误行")
            reporter.event("随便什么事件", value=1)  # 命令行不关心事件
        self.assertEqual(out.getvalue(), "普通行\n")
        self.assertEqual(err.getvalue(), "错误行\n")

    def test_line_masks_wifi_passwords(self):
        out = io.StringIO()
        reporter = remapadctl.ConsoleReporter()
        with contextlib.redirect_stdout(out):
            reporter.line("ok netlog cred ssid=slime_nest pass=hunter2")
        self.assertEqual(out.getvalue(), "ok netlog cred ssid=slime_nest pass=***\n")


class MaskSecretsTest(unittest.TestCase):
    """日志脱敏：netlog 的 WiFi 密码换 ***，其余内容（含 SSID）保持原样。"""

    def test_save_command_with_echo_prefix(self):
        self.assertEqual(link.mask_secrets("> netlog save slime_nest ssssssss"),
                         "> netlog save slime_nest ***")

    def test_direct_connect_command_masks_password_keeps_ip_and_port(self):
        self.assertEqual(link.mask_secrets("> netlog slime_nest hunter2 192.168.1.5 9999"),
                         "> netlog slime_nest *** 192.168.1.5 9999")
        self.assertEqual(link.mask_secrets("> netlog slime_nest hunter2"),
                         "> netlog slime_nest ***")
        # 没有回显前缀的行是设备侧输出而非发出的命令，不碰（固件日志不会回显命令）。
        self.assertEqual(link.mask_secrets("netlog slime_nest hunter2 192.168.1.5 9999"),
                         "netlog slime_nest hunter2 192.168.1.5 9999")

    def test_cred_reply_masks_password_keeps_unset_dash(self):
        self.assertEqual(link.mask_secrets("ok netlog cred ssid=slime_nest pass=hunter2"),
                         "ok netlog cred ssid=slime_nest pass=***")
        self.assertEqual(link.mask_secrets("ok netlog cred ssid=- pass=-"),
                         "ok netlog cred ssid=- pass=-")

    def test_subcommands_status_and_usage_lines_stay_untouched(self):
        for line in ("netlog cred", "netlog off", "netlog scan",
                     "netlog state=connected ssid=slime_nest dest=192.168.1.5:9999",
                     "netlog session ssid=slime_nest port=9999 (dest learn)",
                     "err usage: netlog [save <ssid> <password> | cred | scan | scanlist | off]",
                     "ok wifi saved", "ok netlog stopped"):
            self.assertEqual(link.mask_secrets(line), line, line)


class QueueReporterTest(unittest.TestCase):
    def test_records_keep_kind_and_fields(self):
        sink: queue.Queue = queue.Queue()
        reporter = gui.QueueReporter(sink)
        reporter.line("普通行")
        reporter.error("错误行")
        reporter.event("shot_saved", path="a.png", chunks=3)
        kinds = [sink.get_nowait() for _ in range(sink.qsize())]
        self.assertEqual(kinds[0], {"kind": "line", "text": "普通行"})
        self.assertEqual(kinds[1], {"kind": "error", "text": "错误行"})
        self.assertEqual(kinds[2], {"kind": "event", "name": "shot_saved",
                                    "path": "a.png", "chunks": 3})

    def test_session_writes_device_lines_into_the_queue(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(gui.QueueReporter(sink))
        session_obj.handle_text("ok key injected\r\n".encode())
        session_obj.handle_text("state pairing=paired\n\n".encode())
        records = [sink.get_nowait() for _ in range(sink.qsize())]
        self.assertEqual([record["text"] for record in records],
                         ["ok key injected", "state pairing=paired"])

    def test_partial_line_waits_for_the_newline(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(gui.QueueReporter(sink))
        session_obj.handle_text(b"ok half")
        self.assertTrue(sink.empty())
        session_obj.handle_text(b" line\n")
        self.assertEqual(sink.get_nowait()["text"], "ok half line")


class FeedbackWriteBackTest(unittest.TestCase):
    """写回手柄的短写判定：Windows 的 hidapi 把短于描述符声明长度的写回补齐到
    OutputReportByteLength 再交驱动（DS5 蓝牙集合声明 547，0x31 的 78 字节写法
    因此返回 547）——返回值比载荷长是常态，按「不等于载荷长度」判定会把每一次
    写回都记成失败（写回手柄计数恒为零、日志刷短写告警）。"""

    class FakePad:
        def __init__(self, written):
            self.written = written
            self.reports: list[bytes] = []

        def write(self, payload):
            self.reports.append(bytes(payload))
            if isinstance(self.written, Exception):
                raise self.written
            return self.written

    def _session_with_pad(self, written):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(gui.QueueReporter(sink))
        session_obj.pad = self.FakePad(written)
        return session_obj, sink

    def test_padded_write_counts_as_success(self):
        session_obj, sink = self._session_with_pad(547)
        self.assertTrue(session_obj.send_output_report(bytes(78)))
        self.assertEqual(session_obj.writeback_short, 0)
        self.assertTrue(sink.empty())

    def test_short_write_is_counted_and_reported(self):
        session_obj, sink = self._session_with_pad(77)
        self.assertFalse(session_obj.send_output_report(bytes(78)))
        self.assertEqual(session_obj.writeback_short, 1)
        self.assertIn("短写", sink.get_nowait()["text"])


class LocalCommandTest(unittest.TestCase):
    """run_local 收到的是去掉冒号后的文本（pump_commands 已经剥掉 ':'）。"""

    def test_shot_path_keeps_spaces(self):
        session_obj, fake = session()
        session_obj.run_local("shot C:\\my shots\\ui 1.png")
        self.assertEqual(session_obj.shot_path, Path("C:\\my shots\\ui 1.png"))
        self.assertEqual(fake.written, [b"shot\r"])

    def test_default_image_path_comes_from_the_cli_defaults(self):
        session_obj, _link = session()
        self.assertEqual(session_obj.args.image, remapadctl.DEFAULT_IMAGE)

    def test_bad_image_path_raises_instead_of_exiting(self):
        session_obj, _link = session()
        with self.assertRaises(remapadctl.ImageError) as caught:
            # 镜像不合法时抛异常：命令行映射成退出码 2，界面把它标红，都不许直接结束进程。
            session_obj.run_local("ota Z:\\nope\\image.bin")
        self.assertIn("读不到镜像", str(caught.exception))

    def test_help_lists_local_commands(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(gui.QueueReporter(sink))
        session_obj.run_local("help")
        text = "\n".join(sink.get_nowait()["text"] for _ in range(sink.qsize()))
        self.assertIn("本工具命令", text)
        self.assertIn(":shot [路径]", text)

    def test_unknown_local_command_reports_error(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(gui.QueueReporter(sink))
        session_obj.run_local("nope")
        self.assertEqual(sink.get_nowait()["kind"], "error")

    def test_quit_stops_the_session(self):
        session_obj, _link = session()
        session_obj.run_local("quit")
        self.assertTrue(session_obj.stop)


if __name__ == "__main__":
    unittest.main()

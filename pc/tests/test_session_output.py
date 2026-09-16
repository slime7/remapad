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
import remapadgui  # noqa: E402  （界面侧的队列接收器在这里，导入不会建窗口）


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


class QueueReporterTest(unittest.TestCase):
    def test_records_keep_kind_and_fields(self):
        sink: queue.Queue = queue.Queue()
        reporter = remapadgui.QueueReporter(sink)
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
        session_obj, _link = session(remapadgui.QueueReporter(sink))
        session_obj.handle_text("ok key injected\r\n".encode())
        session_obj.handle_text("state pairing=paired\n\n".encode())
        records = [sink.get_nowait() for _ in range(sink.qsize())]
        self.assertEqual([record["text"] for record in records],
                         ["ok key injected", "state pairing=paired"])

    def test_partial_line_waits_for_the_newline(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(remapadgui.QueueReporter(sink))
        session_obj.handle_text(b"ok half")
        self.assertTrue(sink.empty())
        session_obj.handle_text(b" line\n")
        self.assertEqual(sink.get_nowait()["text"], "ok half line")


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
        session_obj, _link = session(remapadgui.QueueReporter(sink))
        session_obj.run_local("help")
        text = "\n".join(sink.get_nowait()["text"] for _ in range(sink.qsize()))
        self.assertIn("本工具命令", text)
        self.assertIn(":shot [路径]", text)

    def test_unknown_local_command_reports_error(self):
        sink: queue.Queue = queue.Queue()
        session_obj, _link = session(remapadgui.QueueReporter(sink))
        session_obj.run_local("nope")
        self.assertEqual(sink.get_nowait()["kind"], "error")

    def test_quit_stops_the_session(self):
        session_obj, _link = session()
        session_obj.run_local("quit")
        self.assertTrue(session_obj.stop)


if __name__ == "__main__":
    unittest.main()

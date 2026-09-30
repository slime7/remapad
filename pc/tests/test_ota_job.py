"""固件升级（OTA）发送端状态机：串口与 WiFi 共用同一套，按「链路会丢包」设计。

UDP 上 BEGIN/END 应答与窗口应答都可能丢：这里把周期重发、迟到的旧应答、
设备侧超时作废后从头重来逐个钉住；串口不丢包，同一状态机原样工作。
UdpLink 的大写入分片（免 IP 分片）也在这里守住。
"""

import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402
import remapadctl  # noqa: E402

#: 1000 字节镜像 = 5 个数据帧，一窗（16 帧）装得下。
IMAGE = bytes((index * 7) % 256 for index in range(1000))


class FakeLink:
    def __init__(self) -> None:
        self.written = b""

    def write(self, data: bytes) -> None:
        self.written += data


class RecordingReporter(remapadctl.Reporter):
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.errors: list[str] = []
        self.events: list[tuple[str, dict]] = []

    def line(self, text: str) -> None:
        self.lines.append(text)

    def error(self, text: str) -> None:
        self.errors.append(text)

    def event(self, name: str, **fields) -> None:
        self.events.append((name, fields))


def make_ack(state: int, code: int, next_seq: int, received: int, version: str = "") -> dict:
    return {
        "state_id": state,
        "code_id": code,
        "next_seq": next_seq,
        "received": received,
        "state": link.OTA_STATE_NAMES.get(state, str(state)),
        "code": link.OTA_CODE_NAMES.get(code, str(code)),
        "version": version,
    }


class UdpDatagramChunkTest(unittest.TestCase):
    """UdpLink.write 的大写入按 DATAGRAM_MAX 切成多个报文（不走 IP 分片）。"""

    def test_large_write_splits_into_datagrams(self):
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[bytes] = []

            def send(self, data: bytes) -> None:
                self.sent.append(bytes(data))

        sock = FakeSocket()
        udp = link.UdpLink.__new__(link.UdpLink)
        udp._sock = sock
        payload = bytes(range(256)) * 12  # 3072 字节（OTA 整窗的量级）
        udp.write(payload)
        self.assertEqual(b"".join(sock.sent), payload)
        self.assertEqual([len(piece) for piece in sock.sent], [1024, 1024, 1024])

    def test_small_write_stays_one_datagram(self):
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[bytes] = []

            def send(self, data: bytes) -> None:
                self.sent.append(bytes(data))

        sock = FakeSocket()
        udp = link.UdpLink.__new__(link.UdpLink)
        udp._sock = sock
        udp.write(b"status\r")
        self.assertEqual(sock.sent, [b"status\r"])


class OtaJobTest(unittest.TestCase):
    def setUp(self) -> None:
        self.link = FakeLink()
        self.reporter = RecordingReporter()
        self.job = remapadctl.OtaJob(IMAGE, "v-test", self.link.write, self.reporter)

    def frames(self):
        frames, _text = link.FrameDecoder().feed(self.link.written)
        return frames

    def frame_types(self):
        return [item[0] for item in self.frames()]

    def data_frames(self):
        return [item for item in self.frames() if item[0] == link.TYPE_OTA_DATA]

    def drop_written(self) -> None:
        self.link.written = b""

    def test_happy_path_is_begin_window_end(self):
        self.job.start(0.0)
        self.assertEqual(self.frame_types(), [link.TYPE_OTA_BEGIN])
        self.drop_written()
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0, version="old"))
        data = self.data_frames()
        self.assertEqual(len(data), 5)
        self.assertEqual(data[-1][1], link.OTA_SLOT_WINDOW_END)  # 末帧带窗口末标记
        self.drop_written()
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 5, 1000))
        self.assertEqual(self.frame_types(), [link.TYPE_OTA_END])
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_DONE, 0, 5, 1000))
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 0)

    def test_begin_resends_periodically_until_ack(self):
        self.job.start(0.0)
        self.drop_written()
        self.job.tick(1.0)  # 重发间隔内静默
        self.assertEqual(self.frames(), [])
        self.job.tick(2.5)  # BEGIN_RETRY_S 到：重发（设备端同尺寸幂等应答）
        self.assertEqual(self.frame_types(), [link.TYPE_OTA_BEGIN])
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.assertEqual(len(self.data_frames()), 5)

    def test_begin_total_timeout_fails(self):
        self.job.start(0.0)
        self.job.tick(remapadctl.OtaJob.BEGIN_ACK_TIMEOUT_S + 1.0)
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)
        self.assertTrue(self.reporter.errors)

    def test_window_loss_resends_whole_window_then_resumes(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.drop_written()
        self.job.deadline = 0.0  # 强制窗口应答超时
        self.job.tick(time.monotonic())
        resent = self.data_frames()
        self.assertEqual(len(resent), 5)
        self.assertEqual(resent[0][3][:2], (0).to_bytes(2, "little"))
        # 设备只认到第 2 帧：ACK 的 next_seq 是续传起点
        self.drop_written()
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 2, 400))
        resumed = self.data_frames()
        self.assertEqual(len(resumed), 3)
        self.assertEqual(resumed[0][3][:2], (2).to_bytes(2, "little"))

    def test_stale_ack_does_not_rollback_progress(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.drop_written()
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 2, 400))
        self.assertEqual(self.job.confirmed, 400)
        self.assertEqual(len(self.data_frames()), 3)
        self.drop_written()
        # 迟到的旧应答（重复 BEGIN 的幂等应答 / 重发窗口的旧 ACK）：不回卷、不重发
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.assertEqual(self.job.confirmed, 400)
        self.assertEqual(self.frames(), [])
        # 无进展的重复应答（设备写 flash 停顿触发的整窗重发，其序号错误应答）
        # 同样不触发窗口重发——否则与设备的限流应答互相放大。
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 3, 2, 400))
        self.assertEqual(self.frames(), [])

    def test_device_idle_timeout_restarts_from_scratch(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 2, 400))
        # 设备侧 5 秒空闲作废（FAILED + TIMEOUT）：自动重发 BEGIN、进度归零
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_FAILED, remapadctl.OTA_CODE_TIMEOUT, 0, 0))
        self.assertFalse(self.job.finished)
        self.assertEqual(self.job.phase, "begin")
        self.assertEqual(self.job.confirmed, 0)
        self.assertEqual(self.frame_types()[-1], link.TYPE_OTA_BEGIN)
        # 重来有上限：到次数后同样的超时改为终止
        for _ in range(remapadctl.OtaJob.MAX_SESSION_RESTARTS):
            self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
            self.job.on_ack(make_ack(remapadctl.OTA_STATE_FAILED, remapadctl.OTA_CODE_TIMEOUT, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_FAILED, remapadctl.OTA_CODE_TIMEOUT, 0, 0))
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)

    def test_device_hard_failure_terminates(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_FAILED, 4, 0, 0))  # FLASH_ERROR
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)

    def test_end_ignores_straggler_data_acks(self):
        """设备校验镜像期间，整窗重发的迟到序号错误应答不能误判成升级失败。"""
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 5, 1000))
        self.drop_written()
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 3, 5, 1000))
        self.assertFalse(self.job.finished)  # 迟到应答被忽略，仍在等 DONE
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_DONE, 0, 5, 1000))
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 0)

    def test_end_retries_until_done_ack(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 5, 1000))
        self.assertEqual(self.frame_types()[-1], link.TYPE_OTA_END)
        self.drop_written()
        self.job.retry_deadline = 0.0  # 强制 END 重发窗口到期（总窗未到）
        self.job.tick(time.monotonic() + 1)
        self.assertEqual(self.frame_types(), [link.TYPE_OTA_END])
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_DONE, 0, 5, 1000))
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 0)

    def test_end_total_timeout_depends_on_lossy(self):
        for lossy, expected in ((False, 1), (True, 0)):
            with self.subTest(lossy=lossy):
                reporter = RecordingReporter()
                job = remapadctl.OtaJob(IMAGE, "v-test", FakeLink().write, reporter, lossy=lossy)
                job.start(0.0)
                job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 0, 0))
                job.on_ack(make_ack(remapadctl.OTA_STATE_RECEIVING, 0, 5, 1000))
                job.deadline = 0.0  # 强制 END 总窗超时（无任何应答）
                job.tick(time.monotonic())
                self.assertTrue(job.finished)
                self.assertEqual(job.exit_code, expected)
                if lossy:
                    self.assertFalse(reporter.errors)  # 不确定完成不按错误刷屏

    def test_begin_refusal_fails_the_job(self):
        self.job.start(0.0)
        self.job.on_ack(make_ack(remapadctl.OTA_STATE_FAILED, 1, 0, 0))  # BUSY
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)


if __name__ == "__main__":
    unittest.main()

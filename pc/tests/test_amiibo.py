"""amiibo 上传的 PC 侧用例：帧载荷编解码、dump 校验与上传状态机。"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402
import remapadctl  # noqa: E402


def make_dump() -> bytes:
    return bytes(index % 256 for index in range(link.AMIIBO_TAG_SIZE))


def make_dump_with_sig() -> bytes:
    """572 字节 dump：镜像 + 尾部 32 字节厂商签名。"""
    return make_dump() + bytes((0xC0 + index) % 256 for index in range(link.AMIIBO_SIG_SIZE))


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


class AmiiboPayloadTest(unittest.TestCase):
    """BEGIN/DATA/ACK 载荷与固件 amiibo_proto.h 的布局逐字节对得上。"""

    def decode_one(self, frame: bytes):
        frames, _ = link.FrameDecoder().feed(frame)
        self.assertEqual(len(frames), 1)
        return frames[0]

    def test_begin_payload_carries_name_and_size(self):
        frame_type, _slot, _seq, payload = self.decode_one(
            link.encode(link.TYPE_AMIIBO_BEGIN, 0, 0, link.amiibo_begin_payload("Alm", 540)))
        self.assertEqual(frame_type, link.TYPE_AMIIBO_BEGIN)
        self.assertEqual(payload, bytes([3]) + b"Alm" + (540).to_bytes(4, "little"))

    def test_begin_payload_rejects_empty_and_overlong_name(self):
        with self.assertRaises(ValueError):
            link.amiibo_begin_payload("", 540)
        with self.assertRaises(ValueError):
            link.amiibo_begin_payload("x" * (link.AMIIBO_NAME_MAX + 1), 540)

    def test_data_payload_keeps_wire_limit(self):
        chunk = b"\x5a" * link.AMIIBO_DATA_MAX
        frame_type, _slot, _seq, payload = self.decode_one(
            link.encode(link.TYPE_AMIIBO_DATA, 0, 0,
                        link.amiibo_data_payload(400, chunk), max_payload=link.WIRE_MAX_PAYLOAD))
        self.assertEqual(frame_type, link.TYPE_AMIIBO_DATA)
        self.assertEqual(payload[:2], (400).to_bytes(2, "little"))
        self.assertEqual(payload[2:], chunk)
        with self.assertRaises(ValueError):
            link.amiibo_data_payload(0, b"\x00" * (link.AMIIBO_DATA_MAX + 1))

    def test_ack_round_trip(self):
        ack = bytes([2, 0]) + (540).to_bytes(4, "little") + bytes([3])
        _frame_type, _slot, _seq, payload = self.decode_one(
            link.encode(link.TYPE_AMIIBO_ACK, 0, 0, ack))
        parsed = link.parse_amiibo_ack(payload)
        self.assertEqual(parsed["state_id"], 2)
        self.assertEqual(parsed["code_id"], 0)
        self.assertEqual(parsed["received"], 540)
        self.assertEqual(parsed["slot"], 3)


class LoadAmiiboTest(unittest.TestCase):
    def test_rejects_wrong_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Alm.bin"
            path.write_bytes(b"\x00" * 512)
            with self.assertRaises(remapadctl.AmiiboError):
                remapadctl.load_amiibo(path)

    def test_accepts_dump_with_signature(self):
        # 572 字节（镜像 + 厂商签名）原样上传：签名段进设备读缓冲头区。
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Bokoblin.bin"
            path.write_bytes(make_dump_with_sig())
            name, data = remapadctl.load_amiibo(path)
            self.assertEqual(name, "Bokoblin")
            self.assertEqual(data, make_dump_with_sig())

    def test_rejects_missing_file(self):
        with self.assertRaises(remapadctl.AmiiboError):
            remapadctl.load_amiibo(Path("Z:/definitely/not/here.bin"))

    def test_name_comes_from_stem_and_stays_within_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Samus_Aran.bin"
            path.write_bytes(make_dump())
            name, data = remapadctl.load_amiibo(path)
            self.assertEqual(name, "Samus_Aran")
            self.assertEqual(data, make_dump())

            # 31 字节上限按 UTF-8 截断，不在多字节字符中间留半个字。
            long_name = "火" * 20
            path = Path(tmp) / f"{long_name}.bin"
            path.write_bytes(make_dump())
            name, _data = remapadctl.load_amiibo(path)
            self.assertLessEqual(len(name.encode("utf-8")), link.AMIIBO_NAME_MAX)
            self.assertEqual(name, "火" * 10)


class AmiiboJobTest(unittest.TestCase):
    def setUp(self) -> None:
        self.link = FakeLink()
        self.reporter = RecordingReporter()
        self.job = remapadctl.AmiiboJob("Alm", make_dump(), self.link.write, self.reporter)

    def frames(self):
        frames, _text = link.FrameDecoder().feed(self.link.written)
        return frames

    def ack(self, state: int, code: int, received: int, slot: int = 0xFF) -> None:
        # parse_amiibo_ack 的产物带文本名；这里按同一构造补齐。
        self.job.on_ack({
            "state_id": state,
            "code_id": code,
            "received": received,
            "slot": slot,
            "state": link.AMIIBO_STATE_NAMES.get(state, str(state)),
            "code": link.AMIIBO_CODE_NAMES.get(code, str(code)),
        })

    def test_upload_sequence_is_begin_data_end(self):
        self.job.start(0.0)
        frames = self.frames()
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][0], link.TYPE_AMIIBO_BEGIN)
        self.assertEqual(frames[0][3], link.amiibo_begin_payload("Alm", 540))

        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 0)
        frames = self.frames()
        data_frames = [item for item in frames if item[0] == link.TYPE_AMIIBO_DATA]
        self.assertEqual([item[3][:2] for item in data_frames],
                         [(0).to_bytes(2, "little"), (200).to_bytes(2, "little"),
                          (400).to_bytes(2, "little")])
        self.assertEqual([len(item[3]) - 2 for item in data_frames], [200, 200, 140])
        self.assertEqual([item[3][2:] for item in data_frames],
                         [make_dump()[0:200], make_dump()[200:400], make_dump()[400:540]])

        # 中途的 ACK 不触发重发（三帧已经全在途）。
        self.link.written = b""
        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 200)
        self.assertEqual([item[0] for item in self.frames()], [])

        # 收满即发 END；设备回 DONE 后任务收口并带出槽位号。
        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 540)
        frames = self.frames()
        self.assertEqual([item[0] for item in frames], [link.TYPE_AMIIBO_END])
        self.ack(remapadctl.AMIIBO_STATE_DONE, 0, 540, slot=2)
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 0)
        self.assertIn("槽位 2", " ".join(self.reporter.lines))

    def test_timeout_resends_from_last_confirmed_byte(self):
        self.job.start(0.0)
        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 0)
        sent_all = len(self.frames())

        self.job.received = 200
        self.job.deadline = 0.0
        self.job.tick(100.0)
        resent = self.frames()[sent_all:]
        self.assertEqual([item[3][:2] for item in resent],
                         [(200).to_bytes(2, "little"), (400).to_bytes(2, "little")])

    def test_begin_refusal_fails_the_job(self):
        self.job.start(0.0)
        self.ack(remapadctl.AMIIBO_STATE_FAILED, link.AMIIBO_CODE_BAD_HEADER, 0)
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)
        self.assertTrue(self.reporter.errors)

    def test_store_failure_at_end_fails_the_job(self):
        self.job.start(0.0)
        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 0)
        self.ack(remapadctl.AMIIBO_STATE_RECEIVING, 0, 540)
        self.ack(remapadctl.AMIIBO_STATE_FAILED, link.AMIIBO_CODE_STORE_ERROR, 540)
        self.assertTrue(self.job.finished)
        self.assertEqual(self.job.exit_code, 1)


if __name__ == "__main__":
    unittest.main()

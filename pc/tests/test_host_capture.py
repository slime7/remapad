"""主机原始输出采集的 PC 侧纯逻辑：采集帧解析与落盘文件格式。

解析或行格式写错会表现为「抓出来的文件对不上设备现场」——通道认错、
截断标记丢掉、时间列缺失都会让抓包失去对账价值，在这里逐条钉住。
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）
from remapadctl import HostCaptureSink  # noqa: E402


def rumble_payload() -> bytes:
    """一条震动通道的采集载荷：通道 + 长度 + 32 字节 LRA 参数包样例。"""
    data = bytes([0x7C, 0x04, 0x80, 0x01, 0x97, 0x63]) + b"\x00" * 26
    return bytes([0x12, len(data)]) + data  # 0x12 = 震动输出通道（GATT 句柄低字节）


class ParseHostRawTest(unittest.TestCase):
    def test_rumble_record_round_trip(self):
        parsed = link.parse_host_raw(rumble_payload())
        self.assertEqual(parsed["channel"], 0x12)
        self.assertEqual(parsed["name"], "rumble")
        self.assertFalse(parsed["truncated"])
        self.assertEqual(len(parsed["data"]), 32)
        self.assertEqual(parsed["data"][:2], b"\x7c\x04")

    def test_truncation_flag_and_unknown_channel(self):
        # 标志字节 bit7 = 截断，低 7 位 = 数据长度；未登记通道按 ch-<hex> 显示。
        payload = bytes([0x31, 0x80 | 0x02, 0xAA, 0xBB])
        parsed = link.parse_host_raw(payload)
        self.assertTrue(parsed["truncated"])
        self.assertEqual(len(parsed["data"]), 2)
        self.assertEqual(parsed["name"], "ch-31")

    def test_payload_shorter_than_header_is_rejected(self):
        with self.assertRaises(ValueError):
            link.parse_host_raw(b"\x12")

    def test_frame_level_round_trip(self):
        # 设备帧 → 解码 → 采集帧解析，整条链路对得上。
        frame = link.encode(link.TYPE_HOST_RAW, 7, 0, rumble_payload(),
                            max_payload=link.WIRE_MAX_PAYLOAD)
        frames, _text = link.FrameDecoder().feed(frame)
        frame_type, slot, _seq, payload = frames[0]
        self.assertEqual(frame_type, link.TYPE_HOST_RAW)
        self.assertEqual(slot, 7)
        self.assertEqual(link.parse_host_raw(payload)["name"], "rumble")


class HostCaptureSinkTest(unittest.TestCase):
    def _sink(self, directory: Path) -> HostCaptureSink:
        return HostCaptureSink(directory / "host-raw.log", started=100.0)

    def test_records_land_as_readable_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            sink = self._sink(Path(tmp))
            sink.open()
            sink.write_record(link.parse_host_raw(rumble_payload()), seq=0, now=100.5)
            sink.write_record(link.parse_host_raw(bytes([0x14, 0x01, 0x09])), seq=1,
                              now=101.25)
            summary = sink.close()
            text = (Path(tmp) / "host-raw.log").read_text(encoding="utf-8")
        self.assertIn("2 条", summary)
        lines = text.strip().splitlines()
        self.assertEqual(len(lines), 4)  # 两行头注释 + 两条记录
        self.assertTrue(lines[0].startswith("# remapad host raw capture"))
        self.assertIn("+0.500s rumble[0x12] seq=000  32B", lines[2])
        self.assertIn("7c 04 80 01 97 63", lines[2])
        self.assertIn("+1.250s cmd[0x14] seq=001   1B 09", lines[3])

    def test_seq_gap_is_counted_not_fatal(self):
        with tempfile.TemporaryDirectory() as tmp:
            sink = self._sink(Path(tmp))
            sink.open()
            sink.write_record(link.parse_host_raw(rumble_payload()), seq=0, now=100.0)
            # 记录号跳到 5：设备侧队列满丢过包，继续往后写而不是中断。
            sink.write_record(link.parse_host_raw(rumble_payload()), seq=5, now=100.1)
            sink.write_record(link.parse_host_raw(rumble_payload()), seq=6, now=100.2)
            summary = sink.close()
        self.assertIn("1 处跳号", summary)
        self.assertIn("3 条", summary)

    def test_close_without_open_is_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            sink = self._sink(Path(tmp))
            self.assertIsNone(sink.close())


if __name__ == "__main__":
    unittest.main()

"""桥接帧编解码：成帧、丢帧重同步与文本旁路（GUI 与命令行共用这一份）。"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import link  # noqa: E402  （先把 pc/ 放进来再导入）


class FrameDecoderTest(unittest.TestCase):
    def decode(self, data: bytes):
        return link.FrameDecoder().feed(data)

    def test_round_trip(self):
        frame = link.encode(link.TYPE_REPORT, 0, 5, b"\x01\x02")
        frames, text = self.decode(frame)
        self.assertEqual(frames, [(link.TYPE_REPORT, 0, 5, b"\x01\x02")])
        self.assertEqual(text, b"")

    def test_accepts_long_payloads(self):
        payload = bytes(range(200))
        frame = link.encode(link.TYPE_OTA_DATA, 0, 3, payload, max_payload=link.WIRE_MAX_PAYLOAD)
        frames, _text = self.decode(frame)
        self.assertEqual(frames[0][3], payload)

    def test_text_passes_through(self):
        frames, text = self.decode(b"cli ready\r\n")
        self.assertEqual(frames, [])
        self.assertEqual(text, b"cli ready\r\n")

    def test_garbage_before_a_frame_is_kept_as_text(self):
        frame = link.encode(link.TYPE_PING, 0, 0, b"\x01")
        frames, text = self.decode(b"\x11" + frame)
        self.assertEqual(text, b"\x11")
        self.assertEqual(len(frames), 1)

    def test_broken_crc_is_not_delivered_as_a_frame(self):
        frame = bytearray(link.encode(link.TYPE_REPORT, 0, 5, b"\x03"))
        frame[-1] ^= 0x5A  # 破坏校验和字节
        frame[-2] ^= 0x11
        frames, text = self.decode(bytes(frame))
        self.assertEqual(frames, [])
        # 坏帧退化成文本而不是被静默丢掉；解码器最多留一个疑似帧头的字节等下一批。
        self.assertLessEqual(len(frame) - len(text), 1)

    def test_frame_split_across_reads(self):
        frame = link.encode(link.TYPE_FEEDBACK, 0, 7, b"\x00\x01\x02")
        decoder = link.FrameDecoder()
        frames, text = decoder.feed(frame[:4])
        self.assertEqual((frames, text), ([], b""))
        frames, _text = decoder.feed(frame[4:])
        self.assertEqual(frames, [(link.TYPE_FEEDBACK, 0, 7, b"\x00\x01\x02")])

    def test_two_frames_in_one_read(self):
        first = link.encode(link.TYPE_PING, 0, 0, b"\x01")
        second = link.encode(link.TYPE_DETACH, 0, 1, b"\x02")
        frames, text = self.decode(first + second)
        self.assertEqual(text, b"")
        self.assertEqual([frame[0] for frame in frames], [link.TYPE_PING, link.TYPE_DETACH])


if __name__ == "__main__":
    unittest.main()

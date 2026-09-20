"""DS5 蓝牙私有触觉流（0x32 报告）的编码与发送：报文布局、CRC32、序号与
渲染声道的黄金断言。参考 SAxense.c（首个与真机互通的公开实现）、SDL 的
Switch 2 驱动与 Linux hid-playstation.c 的 CRC 规则；两条通路都没有真机可测，
字节在这里钉死。
"""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ds5_haptics  # noqa: E402  （先把 pc/ 放进来再导入）

SILENT_PCM = bytes(ds5_haptics.BT_PCM_BYTES)


def reference_crc32(data: bytes) -> int:
    """逐位 CRC-32（反射多项式 0xEDB88320、初值/终值异或 0xFFFFFFFF），
    作为 zlib 之外的独立参照实现。"""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = (crc & 1) * 0xEDB88320
            crc = (crc >> 1) ^ mask
    return crc ^ 0xFFFFFFFF


def silent_side() -> dict:
    return {"count": 0, "keys": ()}


class BuildReportTest(unittest.TestCase):
    def test_report_layout_golden(self):
        """报文逐字段：0x32 头、packet 0x11 配置/序号、packet 0x12 承载 PCM、
        补零到 137 字节。"""
        pcm = bytes(range(64))
        report = ds5_haptics.bt_build_report(pcm, seq=0x2A)
        self.assertEqual(len(report), ds5_haptics.BT_REPORT_LEN)
        self.assertEqual(report[0], 0x32)
        self.assertEqual(report[1], 0x00)  # tag/seq 字节保持 0
        self.assertEqual(report[2], 0x91)  # packet 0x11 + sized 位
        self.assertEqual(report[3], 0x07)
        self.assertEqual(report[4], 0xFE)
        self.assertEqual(report[5:9], b"\x00\x00\x00\x00")
        self.assertEqual(report[9], 0xFF)
        self.assertEqual(report[10], 0x2A)  # 递增序号在 packet 0x11 内
        self.assertEqual(report[11], 0x92)  # packet 0x12 + sized 位
        self.assertEqual(report[12], 0x40)
        self.assertEqual(report[13:77], pcm)
        self.assertEqual(report[77:137], bytes(60))
        self.assertNotEqual(report[137:141], b"\x00\x00\x00\x00")

    def test_crc_covers_header_and_payload(self):
        """尾部 CRC32：种子字节 0xA2 先过一遍、覆盖除 CRC 外的 137 字节、
        小端落位——缺它手柄整份报告都不认（与 0x31 同一规则）。"""
        report = bytearray(ds5_haptics.bt_build_report(SILENT_PCM, seq=1))
        want = reference_crc32(bytes([0xA2]) + bytes(report[:137]))
        got = int.from_bytes(report[137:141], "little")
        self.assertEqual(got, want)

    def test_pcm_change_changes_crc(self):
        """CRC 跟着报告体走：PCM 变了校验必须跟着变。"""
        a = ds5_haptics.bt_build_report(SILENT_PCM, seq=0)
        b = ds5_haptics.bt_build_report(bytes([1]) + SILENT_PCM[1:], seq=0)
        self.assertNotEqual(a[137:141], b[137:141])

    def test_sequence_increments_and_wraps(self):
        """序号逐报递增、8 位回绕。"""
        first = ds5_haptics.bt_build_report(SILENT_PCM, seq=0xFF)
        second = ds5_haptics.bt_build_report(SILENT_PCM, seq=0x00)
        self.assertEqual(first[10], 0xFF)
        self.assertEqual(second[10], 0x00)

    def test_rejects_wrong_pcm_size(self):
        with self.assertRaises(ValueError):
            ds5_haptics.bt_build_report(b"\x00" * 63, seq=0)


class RenderPcmTest(unittest.TestCase):
    def test_silence_when_idle(self):
        """没有子帧时触觉 PCM 全零（保持私有通路活跃的静音报文）。"""
        state = ds5_haptics._VoiceState()
        pcm = ds5_haptics.bt_render_pcm(silent_side(), silent_side(), (), state)
        self.assertEqual(pcm, SILENT_PCM)

    def test_tone_hits_both_channels_interleaved(self):
        """子帧按左/右音圈交错落位：单侧发声时另一侧保持静音。"""
        state = ds5_haptics._VoiceState()
        side = {"count": 3, "keys": (((55, 255), (0, 0)),) * 3}
        pcm = ds5_haptics.bt_render_pcm(side, silent_side(), (), state)
        left = pcm[0::2]
        right = pcm[1::2]
        self.assertGreater(max(abs(b - 128 if b > 127 else b) for b in left), 40)
        self.assertEqual(right, bytes(ds5_haptics.BT_FRAMES))
        # s8 承载：任何样本都不得越出 8 位刻度。
        for b in pcm:
            signed = b - 256 if b > 127 else b
            self.assertGreaterEqual(signed, -128)
            self.assertLessEqual(signed, 127)

    def test_sample_segments_reach_the_coils(self):
        """蓝牙上没有扬声器通道：0x32 只承载 2 声道触觉 PCM，发声段由固件
        折进触觉子帧，这里的扬声器音色参数不会悄悄进触觉 PCM。"""
        state = ds5_haptics._VoiceState()
        pcm = ds5_haptics.bt_render_pcm(silent_side(), silent_side(), ((880, 255),), state)
        self.assertEqual(pcm, SILENT_PCM)

    def test_keys_play_in_time_order(self):
        """子帧时间轴在蓝牙流上同样保留：3kHz 下每切片 15 样本。"""
        state = ds5_haptics._VoiceState()
        side = {"count": 3, "keys": (((55, 255), (0, 0)),
                                     ((0, 0), (0, 0)),
                                     ((55, 255), (0, 0)))}
        pcm = ds5_haptics.bt_render_pcm(side, side, (), state)
        left = pcm[0::2]
        self.assertGreater(max(abs(v) for v in left[:15]), 30)
        self.assertEqual(left[15:30], bytes(15))  # 子帧 1：静默切片
        self.assertGreater(max(abs(v) for v in left[30:45]), 30)  # 回绕到子帧 2


class SenderLoopTest(unittest.TestCase):
    def test_sender_pushes_well_formed_reports(self):
        """发送线程按节拍推报：序号逐报递增、每份 CRC 都对得上、错误即收尾。"""
        import threading

        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                if len(self.writes) >= 3:
                    raise OSError("done")

        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device)
        sender.set_params({"hd": {
            "l": {"count": 3, "keys": (((55, 128), (0, 0)),) * 3},
            "r": silent_side(),
            "speaker": (0, 0)}})
        sender._run()  # 第 3 次写回抛 OSError，循环自行退出
        self.assertEqual(len(device.writes), 3)
        for i, report in enumerate(device.writes):
            self.assertEqual(len(report), ds5_haptics.BT_REPORT_LEN)
            self.assertEqual(report[10], i)
            want = reference_crc32(bytes([0xA2]) + report[:137])
            self.assertEqual(int.from_bytes(report[137:141], "little"), want)
            self.assertNotEqual(report[13:77], SILENT_PCM)  # 左侧子帧在震

        # 无声时发静音报文（保持私有通路活跃）。
        sender = ds5_haptics.Ds5HapticsBt(device)
        sender.set_params({})
        device.writes.clear()
        sender._run()
        for report in device.writes:
            self.assertEqual(report[13:77], SILENT_PCM)


class CaptureReplayTest(unittest.TestCase):
    """用 pc/tests/samples/ns2-search-page.capture 回放「查找手柄」页：采样流
    以约 16 Hz 重发定位呼叫 0x02、收尾 0x00——PC 侧的发声段铺色（FEEDBACK
    的 HD 扬声器音色）就来自这条采样流的固件解析结果。"""

    def test_search_page_capture_replays_to_locate_samples(self):
        cap = Path(__file__).resolve().parent / "samples" / "ns2-search-page.capture"
        total = locate = stop = 0
        for line in cap.read_text(encoding="utf-8").splitlines():
            if line.startswith("#") or not line.strip():
                continue
            match = re.search(r"B ((?:[0-9a-f]{2} ?)+)$", line)
            self.assertIsNotNone(match, line)
            payload = bytes.fromhex(match.group(1))
            total += 1
            self.assertEqual(payload[0], 0x00)     # 复合输出的填充字节
            self.assertEqual(payload[1:33], bytes(32))  # 震动段全程静置
            frame = payload[33:]
            self.assertEqual(frame[0], 0x0A)       # 触觉采样命令
            sample = frame[8]
            if sample == 0x02:
                locate += 1
            elif sample == 0x00:
                stop += 1
        self.assertEqual(total, 223)
        self.assertEqual(locate, 207)
        self.assertEqual(stop, 16)


if __name__ == "__main__":
    unittest.main()

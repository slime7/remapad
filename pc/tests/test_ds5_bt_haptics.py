"""DS5 蓝牙私有触觉流（0x32 报告）的编码与发送：报文布局、CRC32、序号与
渲染声道的黄金断言。参考 SAxense.c（首个与真机互通的公开实现）、SDL 的
Switch 2 驱动与 Linux hid-playstation.c 的 CRC 规则；两条通路都没有真机可测，
字节在这里钉死。
"""

import math
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
        """报文逐字段（SAxense.c 的真机互通布局，共 142 字节）：0x32 头、
        packet 0x11 配置/序号、packet 0x12 承载 PCM、补零到 138 字节，
        尾部 4 字节 CRC。"""
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
        self.assertEqual(report[77:138], bytes(61))
        self.assertNotEqual(report[138:142], b"\x00\x00\x00\x00")

    def test_crc_covers_header_and_payload(self):
        """尾部 CRC32：种子字节 0xA2 先过一遍、覆盖除 CRC 外的 138 字节
        （Report ID + 全部报文体）、小端落位——缺它或错位，手柄整份报告
        都不认（与 0x31 同一规则）。"""
        report = bytearray(ds5_haptics.bt_build_report(SILENT_PCM, seq=1))
        want = reference_crc32(bytes([0xA2]) + bytes(report[:138]))
        got = int.from_bytes(report[138:142], "little")
        self.assertEqual(got, want)

    def test_pcm_change_changes_crc(self):
        """CRC 跟着报告体走：PCM 变了校验必须跟着变。"""
        a = ds5_haptics.bt_build_report(SILENT_PCM, seq=0)
        b = ds5_haptics.bt_build_report(bytes([1]) + SILENT_PCM[1:], seq=0)
        self.assertNotEqual(a[138:142], b[138:142])

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

    def test_speaker_segment_reaches_the_coils(self):
        """蓝牙上没有扬声器通道：发声段折进两侧音圈 PCM——与 USB 直插的音圈
        行为一致，而不是把发声段静默丢掉。"""
        state = ds5_haptics._VoiceState()
        pcm = ds5_haptics.bt_render_pcm(silent_side(), silent_side(), ((500, 255),), state)
        self.assertNotEqual(pcm, SILENT_PCM)
        left = [b - 256 if b > 127 else b for b in pcm[0::2]]
        right = [b - 256 if b > 127 else b for b in pcm[1::2]]
        self.assertGreater(max(abs(v) for v in left), 40)
        self.assertGreater(max(abs(v) for v in right), 40)

    def test_speaker_matches_usb_coil_scale(self):
        """发声段在两条承载通路上行为一致：蓝牙音圈与 USB 音圈渲染同一份
        折进音色，幅度按各自峰值刻度（24000/127）等比。包络在两条通路上
        同步推进，取进入稳态的第二窗做比较（起音段不参与刻度对比）。"""
        speaker = ((500, 255),)

        def rms(values):
            return math.sqrt(sum(v * v for v in values) / len(values))

        state = ds5_haptics._VoiceState()
        # 预热越过起音段（3kHz 下一个块即满），比较窗各取整周期数
        # （蓝牙 3 块 = 32ms = 16 周期，USB 6144 帧 = 128ms = 64 周期）。
        for _ in range(2):
            pcm_warm = ds5_haptics.bt_render_pcm(silent_side(), silent_side(),
                                                 speaker, state)
        pcm = b"".join(ds5_haptics.bt_render_pcm(silent_side(), silent_side(),
                                                 speaker, state) for _ in range(3))
        bt_left = [b - 256 if b > 127 else b for b in pcm[0::2]]

        audio = ds5_haptics.Ds5HapticsAudio()
        audio.set_params({"hd": {"l": silent_side(), "r": silent_side(),
                                 "speaker": (500, 255)}})
        frames = 6144
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)  # 预热一窗越过 48kHz 起音段
        out = bytearray(frames * ds5_haptics.CHANNELS * 2)
        audio._callback(out, frames, None, None)
        block = memoryview(out).cast("h")
        usb_left = [block[i * 4 + 2] for i in range(frames)]

        ratio = rms(usb_left) / rms(bt_left)
        want = ds5_haptics.AMP_PEAK_USB / ds5_haptics.AMP_PEAK_BT
        self.assertAlmostEqual(ratio, want, delta=want * 0.02)

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
            want = reference_crc32(bytes([0xA2]) + report[:138])
            self.assertEqual(int.from_bytes(report[138:142], "little"), want)
            self.assertNotEqual(report[13:77], SILENT_PCM)  # 左侧子帧在震

        # 无声时发静音报文（保持私有通路活跃）。
        sender = ds5_haptics.Ds5HapticsBt(device)
        sender.set_params({})
        device.writes.clear()
        sender._run()
        for report in device.writes:
            self.assertEqual(report[13:77], SILENT_PCM)

    def test_write_failure_notifies_the_session(self):
        """写回被拒（Windows 长度校验等）时通知会话回落 HID 震动，而不是
        让蓝牙整路静默地哑掉。"""
        import threading

        class RejectingDevice:
            def write(self, report):
                raise OSError("write rejected")

        losses = []
        sender = ds5_haptics.Ds5HapticsBt(RejectingDevice(),
                                          on_error=losses.append)
        sender.set_params({})
        sender._run()
        self.assertEqual(len(losses), 1)
        self.assertIsInstance(losses[0], OSError)

    def test_reports_go_out_raw_saxense_length(self):
        """0x32 报文按 SAxense 的 142 字节原始形态直写、绝不填充：蓝牙报告
        描述符（nondebug/dualsense 实测）声明 0x32 为 141 字节数据（报告 ID +
        137 报文体 + 4 CRC = 142），547 是同族 0x39 的长度——零填充到 547 送出
        的是变长报文，手柄对它无反应；Windows 接受按报告 ID 声明长度的短写
        （0x31 的 78 字节写法同理）。"""
        import threading

        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                if len(self.writes) >= 2:
                    raise OSError("done")

        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device)
        sender.set_params({})
        sender._run()
        self.assertEqual(len(device.writes), 2)
        for i, report in enumerate(device.writes):
            self.assertEqual(len(report), ds5_haptics.BT_REPORT_LEN)
            self.assertEqual(report, ds5_haptics.bt_build_report(SILENT_PCM, i))

        # 填充入口已删：构造器不再收 output_len，报文也不会被拉长。
        self.assertFalse(hasattr(ds5_haptics, "bt_report_windows_pad"))
        self.assertFalse(hasattr(ds5_haptics, "windows_output_report_len"))

    def test_s8_conversion_saturates_instead_of_wrapping(self):
        """蓝牙 PCM 的 s8 换算按饱和处理：音圈叠加喇叭折进后超过 s8 量程时
        夹到 -128/127，而不是按 int16 低位回卷（-32768 的低位是 0x00，
        满幅负样本会变成静音）。"""
        self.assertEqual(ds5_haptics.to_s8(127), 127)
        self.assertEqual(ds5_haptics.to_s8(-128), -128)
        self.assertEqual(ds5_haptics.to_s8(300), 127)
        self.assertEqual(ds5_haptics.to_s8(-300), -128)
        self.assertEqual(ds5_haptics.to_s8(-32768), -128)
        self.assertEqual(ds5_haptics.to_s8(0), 0)


class BuildBt36ReportTest(unittest.TestCase):
    """0x36 报文（DS5Dongle/vds 的蓝牙触觉+喇叭形态，398 字节）：配置包 +
    63 字节状态块 + 64 字节触觉 PCM + 200 字节 Opus 喇叭块 + CRC32。"""

    def test_report_layout_golden(self):
        pcm = bytes(range(64))
        speaker = bytes([0xAA]) * ds5_haptics.BT36_SPEAKER_BYTES
        report = ds5_haptics.bt36_build_report(pcm, speaker, report_seq=0x5,
                                               packet_seq=0xBC)
        self.assertEqual(len(report), ds5_haptics.BT36_REPORT_LEN)
        self.assertEqual(report[0], 0x36)
        self.assertEqual(report[1], 0x50)  # 报告序号在高半字节
        self.assertEqual(report[2], 0x91)  # 配置包 0x11 + sized
        self.assertEqual(report[3], 7)
        self.assertEqual(report[4], 0xFF)  # 音频段全开（vds 实发值）
        self.assertEqual(report[5:10], bytes([64] * 5))  # 音频缓冲长度
        self.assertEqual(report[10], 0xBC)  # 配置包滚动序号
        self.assertEqual(report[11], 0x90)  # 状态块 0x10 + sized
        self.assertEqual(report[12], 63)
        self.assertEqual(report[13:76], ds5_haptics.BT36_STATE)
        self.assertEqual(report[76], 0x92)  # 触觉包 0x12 + sized
        self.assertEqual(report[77], 64)
        self.assertEqual(report[78:142], pcm)
        self.assertEqual(report[142], 0x93)  # 手柄喇叭 0x13 + sized
        self.assertEqual(report[143], ds5_haptics.BT36_SPEAKER_BYTES)
        self.assertEqual(report[144:344], speaker)
        self.assertEqual(report[344:394], bytes(50))  # 尾部保留区

    def test_crc_covers_body(self):
        """CRC32 规则与 0x31/0x32 同一条：种子 0xA2 先过、覆盖除 CRC 外全部
        字节、小端落位（vds 的 kOutputCrcSeed = zlib.crc32(b"\\xA2")）。"""
        pcm = bytes(64)
        speaker = bytes(ds5_haptics.BT36_SPEAKER_BYTES)
        report = ds5_haptics.bt36_build_report(pcm, speaker, report_seq=0,
                                               packet_seq=0)
        want = reference_crc32(bytes([0xA2]) + report[:-4])
        self.assertEqual(int.from_bytes(report[-4:], "little"), want)

    def test_state_block_keeps_leds_to_the_state_reports(self):
        """状态块沿用 vds 运行态（喇叭音量 100、输出路径钉手柄喇叭、触觉走
        音频块），但灯条与玩家灯字节全零：手柄的灯归固件的 0x31 写回管，
        0x36 不掺和。输出路径不路由到手柄喇叭的话，喇叭块会播进没插的
        耳机口（实机：0x36 触觉可达而喇叭无声）。"""
        state = ds5_haptics.BT36_STATE
        self.assertEqual(len(state), 63)
        self.assertEqual(state[0], 0xFD)  # 音频各段使能 + 喇叭音量更新
        self.assertEqual(state[4], 0x7F)  # 耳机音量缺省
        self.assertEqual(state[5], 100)   # 喇叭音量 = PS5 缺省档
        self.assertEqual(state[7], 0x39)  # 输出路径 = 手柄喇叭（0x30 位段）
        self.assertEqual(state[43:47], bytes(4))  # 玩家灯与灯条 RGB 全零

    def test_sender_uses_bt36_with_speaker_encoder(self):
        """speaker 模式的发送线程按 10ms 节拍发 0x36：触觉块照常渲染，喇叭块
        来自编码器（假编码器验证调用即可），序号半字节逐报递增。"""
        import threading

        class FakeEncoder:
            def __init__(self) -> None:
                self.chunks = []

            def encode(self, pcm: bytes) -> bytes:
                self.chunks.append(pcm)
                return bytes([0x55]) * ds5_haptics.BT36_SPEAKER_BYTES

        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                if len(self.writes) >= 3:
                    raise OSError("done")

        encoder = FakeEncoder()
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=encoder)
        sender.set_params({"hd": {"l": {"count": 0, "keys": ()},
                                  "r": {"count": 0, "keys": ()},
                                  "speaker": (880, 255)}})
        sender._run()
        self.assertEqual(len(device.writes), 3)
        self.assertEqual(len(encoder.chunks), 3)
        for i, report in enumerate(device.writes):
            self.assertEqual(len(report), ds5_haptics.BT36_REPORT_LEN)
            self.assertEqual(report[0], 0x36)
            self.assertEqual(report[1], (i & 0xF) << 4)
            self.assertEqual(report[144:344],
                             bytes([0x55]) * ds5_haptics.BT36_SPEAKER_BYTES)

    def test_speaker_encoder_unavailable_falls_back_to_0x32(self):
        """没有 Opus 编码器（PyAV 缺失等）：speaker 模式回落 0x32 音圈流，
        发声段折进音圈，蓝牙不至于整路哑掉。"""
        sender = ds5_haptics.Ds5HapticsBt(object(), speaker_encoder=None)
        self.assertFalse(sender.speaker_active)
        self.assertEqual(sender._interval_s, ds5_haptics.BT_INTERVAL_S)

    def test_speaker_pcm_renders_48k_blocks(self):
        """48kHz 喇叭块的哑渲染：10ms = 480 帧，发声段音色爬起音包络，
        静默段归零；相位与 3kHz 音圈通路的状态分开（互不拖拽）。"""
        state = ds5_haptics._VoiceState()
        silence = ds5_haptics.render_speaker_48k((0, 0), state)
        self.assertEqual(len(silence), ds5_haptics.BT36_SPEAKER_FRAMES * 4)
        self.assertEqual(bytes(silence), bytes(len(silence)))

        tone = ds5_haptics.render_speaker_48k((880, 255), state)
        values = [int.from_bytes(tone[i:i + 2], "little", signed=True)
                  for i in range(0, len(tone), 2)]
        self.assertTrue(any(abs(v) > 10000 for v in values))

    def test_bt36_only_while_speaker_has_content(self):
        """0x36 只在喇叭有内容（含收音尾）时上：满速 0x36 ≈ 40KB/s，Windows
        蓝牙 HID 链路长时间扛不住（实机：接入即断链、重连后报文才落地、触发
        触控板幻手势）。平时与只有触觉时都走 0x32（发声段折进音圈兜底），
        喇叭有音量才切 0x36，退回后 0x32 的折进把发声段接回来。"""
        import threading

        class FakeEncoder:
            def encode(self, pcm: bytes) -> bytes:
                return bytes(ds5_haptics.BT36_SPEAKER_BYTES)

        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                if len(self.writes) >= 3:
                    raise OSError("done")

        # 无内容：全走 0x32。
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=FakeEncoder())
        sender.set_params({})
        sender._run()
        self.assertEqual({len(r) for r in device.writes}, {ds5_haptics.BT_REPORT_LEN})

        # 只有触觉、喇叭静默：仍是 0x32。
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=FakeEncoder())
        sender.set_params({"hd": {"l": {"count": 1, "keys": (((135, 200), (0, 0)),)},
                                  "r": {"count": 0, "keys": ()},
                                  "speaker": (0, 0)}})
        sender._run()
        self.assertEqual({len(r) for r in device.writes}, {ds5_haptics.BT_REPORT_LEN})

        # 喇叭有音量：切 0x36，喇叭块与触觉块都在。
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=FakeEncoder())
        sender.set_params({"hd": {"l": {"count": 0, "keys": ()},
                                  "r": {"count": 0, "keys": ()},
                                  "speaker": (880, 255)}})
        sender._run()
        self.assertEqual({len(r) for r in device.writes},
                         {ds5_haptics.BT36_REPORT_LEN})
        for report in device.writes:
            self.assertEqual(report[0], ds5_haptics.BT36_REPORT_ID)
            self.assertEqual(report[142], 0x93)  # 手柄喇叭 + sized


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

"""DS5 蓝牙私有触觉流（0x32 报告）的编码与发送：报文布局、CRC32、序号与
渲染声道的黄金断言。参考 SAxense.c（公开的互通实现）、SDL 的
Switch 2 驱动与 Linux hid-playstation.c 的 CRC 规则；字节在这里钉死。
"""

import math
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ds5_haptics  # noqa: E402  （先把 pc/ 放进来再导入）

try:
    import av  # noqa: F401  （只在这些用例里用，缺依赖就跳过）
    HAS_AV = True
except Exception:  # noqa: BLE001 - 缺 PyAV/libopus 都按不可用处理
    HAS_AV = False

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
        """报文逐字段（SAxense.c 的互通布局，共 142 字节）：0x32 头、
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

    def test_short_burst_fades_out_smoothly_instead_of_hard_cut(self):
        """短震动的收尾是平滑有界的收音尾，不是块对齐硬切：主机收震后第一块
        仍有声（只占一块的短震动不被截没）、收音时长内落回静音（结束及时，
        不无限拖长）。"""
        state = ds5_haptics._VoiceState()
        burst = {"count": 1, "keys": (((55, 255), (0, 0)),)}
        silent = silent_side()

        def left_peak(block: bytes) -> int:
            values = [b - 256 if b > 127 else b for b in block[0::2]]
            return max(abs(v) for v in values)

        self.assertNotEqual(ds5_haptics.bt_render_pcm(burst, silent, (), state),
                            SILENT_PCM)
        tail = [ds5_haptics.bt_render_pcm(silent, silent, (), state)
                for _ in range(3)]
        self.assertGreater(left_peak(tail[0]), 20)   # 收震后仍在收音尾
        self.assertGreater(left_peak(tail[1]), 0)    # 尾内连续衰减
        self.assertEqual(left_peak(tail[2]), 0)      # 两个块内落回静音

    def test_new_burst_after_silence_starts_from_the_first_subframe(self):
        """整段静默后的新震动从子帧 0 起播：强子帧立刻出去，而不是从上一段
        震动停下的游标位置续播（那会让短震动的起拍落后最多两个子帧）。"""
        state = ds5_haptics._VoiceState()
        burst = {"count": 3, "keys": (((55, 255), (0, 0)),
                                      ((0, 0), (0, 0)),
                                      ((0, 0), (0, 0)))}
        silent = silent_side()
        ds5_haptics.bt_render_pcm(burst, silent, (), state)
        ds5_haptics.bt_render_pcm(burst, silent, (), state)
        ds5_haptics.bt_render_pcm(silent, silent, (), state)
        ds5_haptics.bt_render_pcm(silent, silent, (), state)
        pcm = ds5_haptics.bt_render_pcm(burst, silent, (), state)
        left = [b - 256 if b > 127 else b for b in pcm[0::2]]
        self.assertGreater(max(abs(v) for v in left[:4]), 20)

    def test_gate_saturates_for_continuous_rumble(self):
        """连续震动时包络门饱和在满幅：插值只平滑沿，不压稳态强度。"""
        state = ds5_haptics._VoiceState()
        burst = {"count": 3, "keys": (((55, 255), (0, 0)),) * 3}
        for _ in range(3):
            pcm = ds5_haptics.bt_render_pcm(burst, burst, (), state)
        left = [b - 256 if b > 127 else b for b in pcm[0::2]]
        self.assertGreater(max(abs(v) for v in left), 120)


class SenderLoopTest(unittest.TestCase):
    def test_engagement_is_visible_in_the_log(self):
        """「私有流是否真的在驱动」只有日志能看出来：开关打开与线程启动只说明
        通路就绪，收到主机的 HD 子帧才是真的开始驱动音圈——这一行只在
        第一次接到内容时打一条。"""
        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                raise OSError("done")

        class Reporter:
            def __init__(self) -> None:
                self.lines = []
                self.errors = []

            def line(self, text):
                self.lines.append(text)

            def error(self, text):
                self.errors.append(text)

        reporter = Reporter()
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, reporter)
        sender.set_params({"hd": {"l": {"count": 3, "keys": (((55, 128), (0, 0)),) * 3},
                                  "r": silent_side(),
                                  "speaker": (0, 0)}})
        sender._run()
        self.assertEqual(len(reporter.lines), 1)
        self.assertIn("HD 子帧", reporter.lines[0])
        self.assertIn("0x32", reporter.lines[0])
        # 同一行带首帧的子帧档位：靠它区分「固件没放大」（增益 5 上下）与
        # 「标定值」（增益 20 上下）。
        self.assertIn("55Hz/128", reporter.lines[0])
        self.assertIn("静默", reporter.lines[0])

        # 没有 HD 段（老固件/未接入）不冒充「已驱动」。
        reporter.lines.clear()
        sender = ds5_haptics.Ds5HapticsBt(device, reporter)
        sender.set_params({"hd": None})
        sender._stop.set()
        sender._run()
        self.assertEqual(reporter.lines, [])

    def test_short_write_is_counted_and_reported(self):
        """短写（hidapi 返回值 < 报告长度）要计数并提示：只捕异常会把「驱动没收
        下这一拍」当成成功——蓝牙会频繁漏震而统计一切正常。"""
        class ShortDevice:
            def __init__(self) -> None:
                self.writes = 0

            def write(self, report):
                self.writes += 1
                if self.writes >= 2:
                    raise OSError("done")
                return len(report) - 1  # 少一个字节 = 这一拍没出去

        class Reporter:
            def __init__(self) -> None:
                self.lines = []

            def line(self, text):
                self.lines.append(text)

            def error(self, text):
                self.lines.append(text)

        reporter = Reporter()
        sender = ds5_haptics.Ds5HapticsBt(ShortDevice(), reporter)
        sender.set_params({"hd": {"l": {"count": 1, "keys": (((135, 200), (0, 0)),)},
                                  "r": silent_side(),
                                  "speaker": (0, 0)}})
        sender._run()
        self.assertTrue(any("短写" in line for line in reporter.lines))
        self.assertIn("短写 1 份", sender.stats())

    def test_padded_write_is_not_a_short_write(self):
        """补齐后的写回不算短写：Windows 的 hidapi 把短于描述符声明长度的写回
        补齐到 OutputReportByteLength（DS5 蓝牙集合声明 547）再交驱动，返回值比
        报告长是常态——按「不等于报告长度」判定会每拍重发一次，音圈 PCM 被双倍
        喂进控制器的队列（触觉整段失真、断续）。"""
        class PaddedDevice:
            def __init__(self) -> None:
                self.writes = 0

            def write(self, report):
                self.writes += 1
                if self.writes >= 2:
                    raise OSError("done")
                return 547  # 补齐到描述符声明的输出报告长度

        sender = ds5_haptics.Ds5HapticsBt(PaddedDevice(), None)
        sender.set_params({"hd": {"l": {"count": 1, "keys": (((135, 200), (0, 0)),)},
                                  "r": silent_side(),
                                  "speaker": (0, 0)}})
        sender._run()
        self.assertNotIn("短写", sender.stats())
        self.assertIn("写回统计：1 份", sender.stats())

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

        # 空闲整流停发：蓝牙无线电是公共介质，常驻空包会和同频段设备互相
        # 干扰，静默期一报不发。
        sender = ds5_haptics.Ds5HapticsBt(device)
        sender.set_params({})
        device.writes.clear()
        sender._stop.set()  # 空闲路径不写回：置停止位让循环退出
        sender._run()
        self.assertEqual(device.writes, [])

    def test_new_content_wakes_the_idle_sender(self):
        """新内容到达要唤醒空闲的发送线程：整流停发期间的第一拍震动不等
        20ms 兜底轮询才被看见——短震动的启动延迟少掉一个轮询拍。"""
        sender = ds5_haptics.Ds5HapticsBt(object())
        try:
            sender.set_params({})
            self.assertFalse(sender._wake.is_set())
            sender.set_params({"hd": {"l": {"count": 1, "keys": (((55, 200), (0, 0)),)},
                                      "r": silent_side(),
                                      "speaker": (0, 0)}})
            self.assertTrue(sender._wake.is_set())
            sender._wake.clear()
            sender.set_params({"hd": {"l": silent_side(), "r": silent_side(),
                                      "speaker": (500, 255)}})
            self.assertTrue(sender._wake.is_set())  # 发声段（折进音圈）也算内容
        finally:
            sender.stop()

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
        sender.set_params({"hd": {
            "l": {"count": 1, "keys": (((55, 128), (0, 0)),)},
            "r": silent_side(),
            "speaker": (0, 0)}})
        sender._run()
        self.assertEqual(len(losses), 1)
        self.assertIsInstance(losses[0], OSError)

    def test_reports_go_out_raw_saxense_length(self):
        """0x32 报文按 SAxense 的 142 字节原始形态直写、绝不填充：蓝牙报告
        描述符（nondebug/dualsense）声明 0x32 为 141 字节数据（报告 ID +
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
        sender.set_params({"hd": {
            "l": {"count": 1, "keys": (((135, 128), (0, 0)),)},
            "r": silent_side(),
            "speaker": (0, 0)}})
        sender._run()
        self.assertEqual(len(device.writes), 2)
        for i, report in enumerate(device.writes):
            self.assertEqual(len(report), ds5_haptics.BT_REPORT_LEN)
            self.assertEqual(report[0], 0x32)
            want = reference_crc32(bytes([0xA2]) + report[:138])
            self.assertEqual(int.from_bytes(report[138:142], "little"), want)

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


class _FakeClock:
    """假时钟：等到期的等待按它推进，用例瞬间跑完一整段节拍。"""

    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


class _GridSender(ds5_haptics.Ds5HapticsBt):
    """假时钟驱动的发送线程：等到期的等待把假时钟推到到期时刻，空闲等待
    推进一个兜底轮询拍（可经 on_idle 在这期间喂新内容）。"""

    def __init__(self, device, clock, on_idle=None, **kwargs) -> None:
        super().__init__(device, clock=clock, **kwargs)
        self.clock = clock
        self.idle_rounds = 0
        self._on_idle = on_idle

    def _wait_until_due(self, due: float) -> bool:
        self.clock.advance(max(0.0, due - self.clock()))
        return self._stop.is_set()

    def _wait_for_content(self, timeout: float) -> None:
        self.idle_rounds += 1
        self.clock.advance(timeout)
        if self._on_idle is not None:
            self._on_idle(self.idle_rounds)
        if self._wake.wait(0):
            self._wake.clear()


class SenderTimingTest(unittest.TestCase):
    """发送节拍：每个报文的发送时刻钉在固定网格上，节拍 = 一报里触觉 PCM 的
    时长（32 帧 / 3000Hz ≈ 10.67ms）。假时钟下写回都落在网格点上，用例几毫秒
    跑完一整段节拍。"""

    @staticmethod
    def _coil_params() -> dict:
        return {"hd": {"l": {"count": 3, "keys": (((55, 200), (0, 0)),) * 3},
                       "r": silent_side(), "speaker": (0, 0)}}

    @staticmethod
    def _speaker_params() -> dict:
        return {"hd": {"l": silent_side(), "r": silent_side(),
                       "speaker": (880, 200)}}

    def test_slow_writes_do_not_stretch_the_beat(self):
        """写回慢的链路上节拍不被写回耗时推长：每次写回花 3ms，相邻两报的
        间隔仍是 10.67ms——周期由网格决定，触觉 PCM 才供得上控制器的
        3kHz 消耗（周期被推长就是短震动被吞、震感被拉长）。"""
        clock = _FakeClock()

        class SlowDevice:
            def __init__(self, clock) -> None:
                self._clock = clock
                self.stamps = []

            def write(self, report):
                self.stamps.append(self._clock.now)
                self._clock.advance(0.003)
                if self._clock.now > 0.5:
                    raise OSError("done")

        device = SlowDevice(clock)
        sender = _GridSender(device, clock)
        sender.set_params(self._coil_params())
        sender._run()
        self.assertGreater(len(device.stamps), 40)
        gaps = [b - a for a, b in zip(device.stamps, device.stamps[1:])]
        for gap in gaps:
            self.assertAlmostEqual(gap, ds5_haptics.BT_INTERVAL_S, places=9)

    def test_pcm_frames_per_second_match_the_carrier_rate(self):
        """每秒送出的触觉 PCM 帧数与承载采样率一致（3000 帧/秒）：0x32 与
        0x36 两条承载都按一报 32 帧的时长出报——出快了就是往控制器的 PCM
        队列里多塞帧（震动被拉长、长震收不住），出慢了就是欠喂。"""

        class FakeEncoder:
            def encode(self, pcm: bytes) -> bytes:
                return bytes(ds5_haptics.BT36_SPEAKER_BYTES)

        class CountingDevice:
            def __init__(self, clock) -> None:
                self._clock = clock
                self.stamps = []

            def write(self, report):
                self.stamps.append(self._clock.now)
                if self._clock.now > 0.5:
                    raise OSError("done")

        lanes = (("0x32", self._coil_params(), {}),
                 ("0x36", self._speaker_params(),
                  {"speaker_encoder": FakeEncoder()}))
        for lane, params, kwargs in lanes:
            with self.subTest(lane=lane):
                clock = _FakeClock()
                device = CountingDevice(clock)
                sender = _GridSender(device, clock, **kwargs)
                sender.set_params(params)
                sender._run()
                self.assertGreater(len(device.stamps), 20)
                span = device.stamps[-1] - device.stamps[0]
                frames = (len(device.stamps) - 1) * ds5_haptics.BT_FRAMES
                self.assertAlmostEqual(frames / span, ds5_haptics.BT_RATE,
                                       delta=ds5_haptics.BT_RATE * 0.002)

    def test_pair_form_lands_two_blocks_per_report(self):
        """成对形态（0x39）：一报 2 块触觉 + 2 帧喇叭、节拍 21.33ms——多带的
        那一块是链路抖动的水垫（单块形态下一拍迟到 10.67ms 就断音），报数减半
        也少一半链路开销；触觉块仍按 64 字节声明长度、两块连着放。"""
        class FakeEncoder:
            def encode(self, pcm: bytes) -> bytes:
                return bytes(ds5_haptics.BT36_SPEAKER_BYTES)

        class CountingDevice:
            def __init__(self, clock) -> None:
                self._clock = clock
                self.reports = []
                self.stamps = []

            def write(self, report):
                self.reports.append(bytes(report))
                self.stamps.append(self._clock.now)
                if self._clock.now > 0.5:
                    raise OSError("done")

        clock = _FakeClock()
        device = CountingDevice(clock)
        sender = _GridSender(device, clock, speaker_encoder=FakeEncoder(), pair=True)
        sender.set_params(self._speaker_params())
        sender._run()
        self.assertGreater(len(device.reports), 5)
        first = device.reports[0]
        self.assertEqual(len(first), ds5_haptics.BT39_REPORT_LEN)
        self.assertEqual(first[0], ds5_haptics.BT39_REPORT_ID)
        self.assertEqual(first[3], 6)  # 配置包长度
        self.assertEqual(first[10], 0x12 | 0x80)
        self.assertEqual(first[11], ds5_haptics.BT_PCM_BYTES)
        self.assertEqual(first[140], 0x13 | 0x80)
        self.assertEqual(first[141], ds5_haptics.BT36_SPEAKER_BYTES)
        gaps = [b - a for a, b in zip(device.stamps, device.stamps[1:])]
        for gap in gaps:
            self.assertAlmostEqual(gap, ds5_haptics.BT39_INTERVAL_S, places=9)
        self.assertIn("目标 21.33ms", sender.stats())

    def test_idle_wake_does_not_backfill_the_grid(self):
        """空闲唤醒后第一拍立刻发出、第二拍起仍按节拍，不连发补报：空闲前的
        网格早就过期，唤醒后一口气连发几份会把控制器的 PCM 队列一次塞满
        （短震动先抢跑后拖尾）。"""
        clock = _FakeClock()
        silent = {"hd": {"l": silent_side(), "r": silent_side(),
                         "speaker": (0, 0)}}
        state = {"wake_at": None}

        class QuietDevice:
            def __init__(self, clock) -> None:
                self._clock = clock
                self.stamps = []

            def write(self, report):
                self.stamps.append(self._clock.now)
                if state["wake_at"] is None and self._clock.now >= 0.2:
                    # 震动停了：等收音尾过期，发送线程转入空闲
                    sender.set_params(silent)
                if (state["wake_at"] is not None and
                        self._clock.now > state["wake_at"] + 0.05):
                    raise OSError("done")

        device = QuietDevice(clock)

        def on_idle(_rounds):
            if state["wake_at"] is None:
                state["wake_at"] = clock.now
                sender.set_params(self._coil_params())

        sender = _GridSender(device, clock, on_idle=on_idle)
        sender.set_params(self._coil_params())
        sender._run()
        self.assertGreater(sender.idle_rounds, 0)
        self.assertIsNotNone(state["wake_at"])
        gaps = [b - a for a, b in zip(device.stamps, device.stamps[1:])]
        idle_at = next(i for i, gap in enumerate(gaps)
                       if gap > 1.5 * ds5_haptics.BT_INTERVAL_S)
        self.assertGreater(len(gaps) - idle_at, 2)  # 唤醒后又跑了几拍
        for gap in gaps[idle_at + 1:]:
            self.assertAlmostEqual(gap, ds5_haptics.BT_INTERVAL_S, places=9)


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
        self.assertEqual(report[4], 0xFE)  # 音频段全开但不开麦克风采集（0xFF 会开双工）
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
        耳机口（0x36 触觉可达而喇叭无声）。"""
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
        class FakeDevice:
            def __init__(self) -> None:
                self.writes = []

            def write(self, report):
                self.writes.append(report)
                raise OSError("done")

        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=None)
        self.assertFalse(sender.speaker_active)
        sender.set_params({"hd": {"l": silent_side(), "r": silent_side(),
                                  "speaker": (880, 255)}})
        sender._run()
        self.assertEqual(len(device.writes), 1)
        self.assertEqual(device.writes[0][0], ds5_haptics.BT_REPORT_ID)
        self.assertNotEqual(device.writes[0][13:77], SILENT_PCM)  # 发声段折进音圈

    def test_speaker_block_carries_one_full_beat(self):
        """喇叭块要装下一整拍的内容：手柄按「一块对一拍」消耗 PCM（480 样本
        铺满 10.667ms 节拍 ≈ 45kHz），公开实现是把主机 512 样本的块重采样成
        480 再编一帧；只送 10ms 的内容会每秒欠喂 6.25%（表现为周期性顿挫）。
        用 880Hz 音的过零间距直接量合成时钟：45kHz 下周期 51.1 样本，按 48kHz
        合成会是 54.5 样本。静默段归零、相位与 3kHz 音圈通路分开。"""
        state = ds5_haptics._VoiceState()
        silence = ds5_haptics.render_speaker_beat((0, 0), state)
        self.assertEqual(len(silence), ds5_haptics.BT36_SPEAKER_FRAMES * 4)
        self.assertEqual(bytes(silence), bytes(len(silence)))
        self.assertEqual(ds5_haptics.BT36_SPEAKER_BEAT_RATE, 45000)

        tone = ds5_haptics.render_speaker_beat((880, 255), state)
        mono = [int.from_bytes(tone[i:i + 2], "little", signed=True)
                for i in range(0, len(tone), 4)]
        self.assertTrue(any(abs(v) > 10000 for v in mono))
        crossings = []
        for i in range(len(mono) - 1):
            if mono[i] < 0 <= mono[i + 1]:
                span = mono[i + 1] - mono[i]
                crossings.append(i - mono[i] / span if span else float(i))
        self.assertGreater(len(crossings), 6)
        periods = [b - a for a, b in zip(crossings, crossings[1:])]
        mean = sum(periods) / len(periods)
        self.assertAlmostEqual(mean, ds5_haptics.BT36_SPEAKER_BEAT_RATE / 880.0,
                               delta=1.0)

    def test_bt36_only_while_speaker_has_content(self):
        """0x36 只在喇叭有内容（含收音尾）时上：满速 0x36 ≈ 40KB/s，Windows
        蓝牙 HID 链路长时间扛不住（接入即断链、重连后报文才落地）。平时与只有
        触觉时都走 0x32（发声段折进音圈兜底），
        喇叭有音量才切 0x36，退回后 0x32 的折进把发声段接回来。"""
        import threading

        class FakeEncoder:
            def encode(self, pcm: bytes) -> bytes:
                return bytes(ds5_haptics.BT36_SPEAKER_BYTES)

        class FakeDevice:
            def __init__(self, limit=3) -> None:
                self.writes = []
                self._limit = limit

            def write(self, report):
                self.writes.append(report)
                if len(self.writes) >= self._limit:
                    raise OSError("done")

        # 无内容：整流停发，一报不发。
        device = FakeDevice()
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=FakeEncoder())
        sender.set_params({})
        sender._stop.set()
        sender._run()
        self.assertEqual(device.writes, [])

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

        # 鸣叫停顿期间（喇叭静默、采样还按着）：过收音尾就整流停发，把空口
        # 还给同频段设备——采样按住期间的满速保温会让同频段设备全程被骚扰；
        # 下一声鸣叫重新进入 0x36。
        import time as time_mod

        device = FakeDevice(limit=1)
        sender = ds5_haptics.Ds5HapticsBt(device, speaker_encoder=FakeEncoder())
        sender.set_params({"hd": {"l": {"count": 0, "keys": ()},
                                  "r": {"count": 0, "keys": ()},
                                  "speaker": (880, 255)}})
        sender._run()  # 响一声（写 1 份后 OSError 退出），盖上发声时间戳
        time_mod.sleep(ds5_haptics.BT36_SPEAKER_TAIL_S + 0.05)
        sender.set_params({"sample": 0x02,
                           "hd": {"l": {"count": 0, "keys": ()},
                                  "r": {"count": 0, "keys": ()},
                                  "speaker": (0, 0)}})
        sender._stop.set()  # 过尾长即空闲：置停止位让循环退出
        device.writes.clear()
        sender._run()
        self.assertEqual(device.writes, [])


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


def _decode_stereo_s16(packet: bytes) -> list[int]:
    """一帧 Opus → 左声道样本（测试用，避开 numpy）。"""
    import av

    ctx = av.CodecContext.create("libopus", "r")
    ctx.sample_rate = 48000
    ctx.format = "s16"
    ctx.layout = "stereo"
    ctx.open()
    frames = ctx.decode(av.Packet(packet))
    assert frames, "解码器没有输出帧"
    # planes[0] 是整块分配缓冲，按帧内实际样本数截断（480 帧 × 2ch × 2B）。
    raw = bytes(frames[0].planes[0])[:frames[0].samples * 4]
    return [int.from_bytes(raw[i:i + 2], "little", signed=True)
            for i in range(0, len(raw), 4)]


def _decode_stream(packets: list[bytes]) -> list[int]:
    """一帧帧喂给同一个解码器再拼左声道：Opus 首帧含建立延迟（预跳），按真实
    用法连续解多帧、估频时丢前段才稳。"""
    out: list[int] = []
    for packet in packets:
        out.extend(_decode_stereo_s16(packet))
    return out


def _dft_peak_hz(mono: list[int], rate: int, lo: float, hi: float, skip: int) -> float:
    """滑动频率点的幅度谱峰值（纯 Python，避开 numpy）：比过零计数抗噪，能分辨
    6.25% 的音高差。"""
    window = mono[skip:]
    best_hz, best_pow = 0.0, -1.0
    hz = lo
    while hz <= hi:
        w = 2 * math.pi * hz / rate
        re = sum(v * math.cos(w * k) for k, v in enumerate(window))
        im = sum(v * math.sin(w * k) for k, v in enumerate(window))
        power = re * re + im * im
        if power > best_pow:
            best_pow, best_hz = power, hz
        hz += 1.0
    return best_hz


@unittest.skipUnless(HAS_AV, "PyAV/libopus 不可用（发声段编不了码）")
class SpeakerPitchRoundTripTest(unittest.TestCase):
    """发声段一帧经真实 Opus 编解码后音高必须对：合成按节拍时钟（45kHz）写
    480 样本、交给声明 48kHz 的编码器，解码回来是 音高×48/45——手柄按「一块对
    一拍」播回（≈45kHz）后正好是原音高。这条同时钉住「一帧装下整拍」与
    「Opus 参数没把音高与时长改掉」。"""

    def test_chirp_keeps_its_pitch_through_opus(self):
        try:
            encoder = ds5_haptics.Bt36OpusEncoder()
        except Exception as exc:  # noqa: BLE001
            self.skipTest(f"Opus 编码器不可用：{exc}")
        for played_hz in (880.0, 1175.0):
            state = ds5_haptics._VoiceState()
            packets = [encoder.encode(ds5_haptics.render_speaker_beat((played_hz, 255), state))
                       for _ in range(8)]
            self.assertEqual({len(p) for p in packets}, {ds5_haptics.BT36_SPEAKER_BYTES})
            mono = _decode_stream(packets)
            self.assertEqual(len(mono), ds5_haptics.BT36_SPEAKER_FRAMES * len(packets))
            beat_hz = played_hz * 48000 / ds5_haptics.BT36_SPEAKER_BEAT_RATE
            # 搜索范围同时罩住「按 48kHz 直采」（原音高）与「按整拍合成」两个
            # 候选位置，谁的能量高谁就是实际内容。
            got = _dft_peak_hz(mono, 48000, lo=played_hz * 0.9, hi=played_hz * 1.2,
                               skip=1600)
            # 音高落在「按整拍合成」的位置（48kHz 声明下比原音高高 6.25%），
            # 而不是按 48kHz 时钟直采的位置——手柄按一块对一拍播回后正好原音高。
            self.assertAlmostEqual(got, beat_hz, delta=beat_hz * 0.02)
            self.assertGreater(abs(got - played_hz), abs(got - beat_hz))


if __name__ == "__main__":
    unittest.main()

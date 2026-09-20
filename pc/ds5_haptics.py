"""DS5 音频触觉的 PC 侧合成（桥接路径）。

DualSense 连在 PC 上时有两条投递通路，共用同一份哑渲染：
- USB 直插：音频接口由 Windows 持有（usbaudio.sys），对它的 4ch 扬声器端点
  开 WASAPI 共享流——频道 3/4（RL/RR）直连左右触觉音圈，频道 1/2 是手柄
  小喇叭（采样提示音的发声段，没有真正的声音时恒零）。
- 蓝牙：HID 之外没有音频接口，触觉走 SAxense 逆向的私有报告 0x32（141 字节
  = 报文头 + packet 0x11 配置/序号 + packet 0x12 承载 64 字节 PCM + 尾部
  CRC32），3000Hz / 2 声道 / 8-bit，每 10.67ms 一报由发送线程推送。

两条通路共用同一份哑渲染，行为一致：NS2 的震动是波形描述，每侧最多 3 个
时序子帧，按时间顺序各播 1/3 周期；采样发声段铺扬声器之外同时折进两侧音圈
（蓝牙没有扬声器通道，音圈是它唯一的载体）。声部参数来自设备的 FEEDBACK 帧
（57 字节 HD 版）：固件已按布局行把子帧序列重整好（震动映音圈、采样发声段
映扬声器，频率落地在固件里算好），这里只做哑渲染——振荡器相位跨块连续，
子帧按 slice 帧数轮播；老固件的 16 字节帧回落两带正弦（扬声器恒零）。
实机验证：共享流 4ch 独立可控、扬声器不漏音。

用 RawOutputStream 而不是 OutputStream：后者的回调走 numpy 数组，而 numpy
的原生扩展在会话进程里加载会卡死（cffi/PortAudio 都正常，仅 numpy 如此，
faulthandler 抓栈定位）；raw 模式回调收字节缓冲，struct 直写 int16，
与固件的 PCM 语义一致。
"""
from __future__ import annotations

import math
import struct
import threading
import time
import zlib

#: 与固件 haptic_synth.c 同刻度：gain 255 的 int16 峰值（USB 承载）。
AMP_PEAK_USB = 24000
#: 蓝牙私有流承载 8-bit PCM，峰值按 s8 上限留 1。
AMP_PEAK_BT = 127
RATE = 48000
CHANNELS = 4
#: 频率缺省值（设备发的落地值理论上不为 0，这里兜底；与固件布局行的
#: lf/hf_default_hz 同源：BlueRetro 驱动常量 0x180/0x1E1 的落地值 80/135Hz）。
FREQ_DEFAULTS = (80.0, 135.0)
#: 每侧时序子帧上限（与固件 PAD_HD_KEY_MAX 一致：NS2 波形规则为 3）。
KEY_MAX = 3
#: 子帧序列的整周期（ms）：3 个子帧各播 1/3，与固件 DS5 布局行的 cycle_ms 同值。
CYCLE_MS = 15.0
_TAU = 2.0 * math.pi


def _clamp16(value: int) -> int:
    return max(-32768, min(32767, value))


def _slice_samples(rate: int) -> int:
    """每个子帧的样本数 = rate × cycle_ms / 1000 / 3（48kHz 下 240、3kHz 下 15）。"""
    return max(1, round(rate * CYCLE_MS / 1000.0 / 3.0))


def _hd_voices(params: dict) -> tuple[dict, dict, tuple] | None:
    """HD 段 → (左子帧序列, 右子帧序列, 扬声器)；没有 HD 段返回 None。
    每侧是 {"count": 有效子帧数, "keys": (((lf,lg),(hf,hg)), ...)}。"""
    hd = params.get("hd")
    if hd is None:
        return None
    return hd["l"], hd["r"], (hd["speaker"],)


def _legacy_voices(params: dict) -> tuple[tuple, tuple, tuple]:
    """老固件 16 字节帧回落两带正弦（扬声器恒零）：每侧两条同时叠加的正弦。"""
    lf_freq = params.get("lf_freq") or FREQ_DEFAULTS
    hf_freq = params.get("hf_freq") or FREQ_DEFAULTS
    lf_amp = params.get("lf_amp") or (0, 0)
    hf_amp = params.get("hf_amp") or (0, 0)
    left = ((lf_freq[0] or FREQ_DEFAULTS[0], lf_amp[0]),
            (hf_freq[0] or FREQ_DEFAULTS[0], hf_amp[0]))
    right = ((lf_freq[1] or FREQ_DEFAULTS[0], lf_amp[1]),
             (hf_freq[1] or FREQ_DEFAULTS[1], hf_amp[1]))
    return left, right, ()


def _render_side(tones: tuple, phases: list[float], frames: int, rate: int, peak: int) -> list[int]:
    """一组同时发声的正弦叠加：相位逐样本推进（换参数不重置，拼接处不跳变）。"""
    gains = [gain * peak / 255.0 for _freq, gain in tones]
    steps = [_TAU * freq / rate for freq, _gain in tones]
    out = [0] * frames
    for i in range(frames):
        total = 0.0
        for k in range(len(tones)):
            if gains[k] <= 0.0:
                continue
            total += gains[k] * math.sin(phases[k])
            phases[k] = (phases[k] + steps[k]) % _TAU
        out[i] = _clamp16(round(total))
    return out


def _render_keys(side: dict, phases: list[float], cursor: list, frames: int,
                 rate: int, peak: int, slice_samples: int) -> list[int]:
    """一条时序子帧序列的渲染：首帧从子帧 0 起播整一切片，其后每
    slice_samples 帧切下一子帧（回绕），有效子帧数之外的切片静默——主机排
    好的时间轴原样保留。相位跨块与跨子帧都连续（切子帧只换频率与增益，
    不重置相位）。"""
    count = side["count"]
    keys = side["keys"]
    lf_phase, hf_phase = phases
    idx, left = cursor
    if left == 0 or idx >= KEY_MAX:
        idx = 0
        left = slice_samples
    out = [0] * frames
    for i in range(frames):
        if left == 0:
            idx = (idx + 1) % KEY_MAX
            left = slice_samples
        left -= 1
        total = 0.0
        if idx < count:
            (lf, lg), (hf, hg) = keys[idx]
            if lg:
                total += lg * peak / 255.0 * math.sin(lf_phase)
                lf_phase = (lf_phase + _TAU * lf / rate) % _TAU
            if hg:
                total += hg * peak / 255.0 * math.sin(hf_phase)
                hf_phase = (hf_phase + _TAU * hf / rate) % _TAU
        out[i] = _clamp16(round(total))
    cursor[0], cursor[1] = idx, left
    phases[0], phases[1] = lf_phase, hf_phase
    return out


class _VoiceState:
    """两侧振荡器相位 + 扬声器相位 + 各侧子帧游标（跨块连续）。"""

    def __init__(self) -> None:
        self.key_phase = [[0.0, 0.0], [0.0, 0.0]]
        self.speaker = [0.0]
        self.cursor = [[0, 0], [0, 0]]  # 每侧 [子帧序号, 距下次切换的样本数]


class Ds5HapticsAudio:
    """一条对着 DualSense 音频端点的 4ch int16 输出流 + 子帧驱动的哑渲染。

    set_params 可从会话循环任意调用（内部加锁），合成跑在音频回调线程里，
    相位与子帧游标逐块推进、换参数不重置（拼接处不跳变）。
    """

    LABEL = "DS5 音频触觉已启用（频道 3/4 触觉、1/2 发声，HID 震动让位）"

    def __init__(self, reporter=None) -> None:
        self._reporter = reporter
        self._lock = threading.Lock()
        self._params: dict = {}
        self._state = _VoiceState()
        self._stream = None

    @property
    def active(self) -> bool:
        return self._stream is not None

    def start(self) -> bool:
        """找到 DualSense 端点并开流；任何一步不成立都返回 False（回落 HID）。"""
        try:
            import sounddevice as sd
        except (ImportError, OSError) as exc:
            self._warn(f"音频触觉依赖不可用（{exc}），回落 HID 震动写回")
            return False
        device = self._find_ds5(sd)
        if device is None:
            self._warn("没找到 DualSense 音频端点，回落 HID 震动写回")
            return False
        try:
            self._stream = sd.RawOutputStream(
                device=device, channels=CHANNELS, samplerate=RATE,
                blocksize=480, dtype="int16", callback=self._callback)
            self._stream.start()
        except Exception as exc:  # noqa: BLE001 - 端点被占/格式不符都按回落处理
            self._warn(f"DualSense 音频端点打开失败（{exc}），回落 HID 震动写回")
            self._stream = None
            return False
        return True

    def stop(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:  # noqa: BLE001 - 收尾路径不抛
                pass
            self._stream = None

    def set_params(self, params: dict) -> None:
        """吃 link.feedback_params 的解析结果：HD 时序子帧（固件已按布局重整）
        或老固件的两带振幅与频率落地值。FEEDBACK 的采样字节带原始采样 ID，
        只供日志展示——发声段由固件按音色表折成扬声器音色随 HD 段下发。"""
        with self._lock:
            self._params = dict(params)

    @staticmethod
    def _find_ds5(sd):
        """在所有 hostapi 里找 DualSense 的输出端点，优先 WASAPI（延迟低、
        通道映射直）。要求至少 4 个输出通道。"""
        fallback = None
        for host in sd.query_hostapis():
            for idx in host["devices"]:
                dev = sd.query_devices(idx)
                if "dualsense" not in dev["name"].lower() or dev["max_output_channels"] < CHANNELS:
                    continue
                if "wasapi" in host["name"].lower():
                    return idx
                if fallback is None:
                    fallback = idx
        return fallback

    def _warn(self, text: str) -> None:
        if self._reporter is not None:
            self._reporter.error(text)

    def _callback(self, outdata, frames, _time_info, status) -> None:
        if status:
            self._warn(f"DS5 音频触觉流异常：{status}")
        with self._lock:
            params = dict(self._params)
        hd = _hd_voices(params)
        peak = AMP_PEAK_USB
        if hd is not None:
            left_v, right_v, speaker = hd
            slice_samples = _slice_samples(RATE)
            left = _render_keys(left_v, self._state.key_phase[0], self._state.cursor[0],
                                frames, RATE, peak, slice_samples)
            right = _render_keys(right_v, self._state.key_phase[1], self._state.cursor[1],
                                 frames, RATE, peak, slice_samples)
        else:
            left_v, right_v, speaker = _legacy_voices(params)
            left = _render_side(left_v, self._state.key_phase[0], frames, RATE, peak)
            right = _render_side(right_v, self._state.key_phase[1], frames, RATE, peak)
        sp = _render_side(speaker, self._state.speaker, frames, RATE, peak)
        # 发声段折进触觉两路：蓝牙上没有扬声器通道，音圈是发声段唯一的载体，
        # 两条承载通路对音圈的驱动保持一致；USB 的频道 1/2 照常加一份真声。
        for i in range(frames):
            left[i] = _clamp16(left[i] + sp[i])
            right[i] = _clamp16(right[i] + sp[i])
        # 小喇叭音色铺频道 1/2；没有真正的声音时两路填充 0 静音。
        block = bytearray(frames * CHANNELS * 2)
        pack_into = struct.pack_into
        off = 0
        for i in range(frames):
            pack_into("<4h", block, off, sp[i], sp[i], left[i], right[i])
            off += CHANNELS * 2
        outdata[:] = bytes(block)


#: 蓝牙私有触觉流（SAxense 逆向）的报文形态：Report ID 0x32、141 字节。
BT_REPORT_LEN = 141
BT_REPORT_ID = 0x32
#: packet 0x12 承载 64 字节 PCM = 32 帧 × 2 声道 × 8-bit，3000Hz。
BT_PCM_BYTES = 64
BT_FRAMES = BT_PCM_BYTES // 2
BT_RATE = 3000
#: 发送节拍：32 帧 / 3000Hz ≈ 10.67ms（约 94 报/秒）。
BT_INTERVAL_S = BT_FRAMES / BT_RATE
#: CRC32 种子字节（PS 输出报告的 hidp 传输头，与 0x31 同一规则）。
BT_CRC_SEED = 0xA2


def bt_build_report(pcm: bytes, seq: int) -> bytes:
    """把 64 字节 PCM（32 帧交错双声道 s8）装进 0x32 私有报告。

    布局（SAxense.c）：[0]=0x32、[1]=tag/seq 字节保持 0（递增序号在 packet
    0x11 内）、[2]=0x91（packet 0x11 + sized 位）、[3]=长度 7、[4:11] = 配置
    `FE 00 00 00 00 FF <seq>`（序号在 [10]，逐报递增）、[11]=0x92（packet
    0x12 + sized）、[12]=0x40、[13:77] = PCM、其后补零到 137 字节，尾部 4 字节
    是 CRC32（种子 0xA2 先过一遍、小端，覆盖前 137 字节）。
    """
    if len(pcm) != BT_PCM_BYTES:
        raise ValueError(f"pcm 需要 {BT_PCM_BYTES} 字节，收到 {len(pcm)}")
    report = bytearray(BT_REPORT_LEN)
    report[0] = BT_REPORT_ID
    report[2] = 0x11 | 0x80  # packet 0x11，sized 位
    report[3] = 0x07
    report[4] = 0xFE
    report[9] = 0xFF
    report[10] = seq & 0xFF
    report[11] = 0x12 | 0x80  # packet 0x12，sized 位
    report[12] = BT_PCM_BYTES
    report[13:13 + BT_PCM_BYTES] = pcm
    crc = zlib.crc32(bytes([BT_CRC_SEED]) + bytes(report[:BT_REPORT_LEN - 4])) & 0xFFFFFFFF
    report[BT_REPORT_LEN - 4:BT_REPORT_LEN] = struct.pack("<I", crc)
    return bytes(report)


def bt_render_pcm(left_v: dict, right_v: dict, speaker: tuple,
                  state: _VoiceState) -> bytes:
    """子帧序列 → 一块 32 帧的触觉 PCM（交错左/右音圈 s8）：蓝牙上没有扬声器
    通道，发声段折进两侧音圈——与 USB 直插的音圈行为一致。"""
    peak = AMP_PEAK_BT
    left = _render_keys(left_v, state.key_phase[0], state.cursor[0],
                        BT_FRAMES, BT_RATE, peak, _slice_samples(BT_RATE))
    right = _render_keys(right_v, state.key_phase[1], state.cursor[1],
                         BT_FRAMES, BT_RATE, peak, _slice_samples(BT_RATE))
    sp = _render_side(speaker, state.speaker, BT_FRAMES, BT_RATE, peak)
    out = bytearray(BT_PCM_BYTES)
    for i in range(BT_FRAMES):
        out[i * 2] = _clamp16(left[i] + sp[i]) & 0xFF
        out[i * 2 + 1] = _clamp16(right[i] + sp[i]) & 0xFF
    return bytes(out)


class Ds5HapticsBt:
    """蓝牙连接的 DualSense 私有触觉流：按 10.67ms 节拍把子帧序列渲染成
    0x32 报告写给已打开的 HID 句柄。空闲时发静音报文保持私有通路活跃；发送
    线程独立于会话主循环（蓝牙 HID 写回慢，不能占桥接热路径）。"""

    LABEL = "DS5 蓝牙触觉流已启用（0x32 私有报文，HID 震动让位）"

    def __init__(self, device, reporter=None) -> None:
        self._device = device
        self._reporter = reporter
        self._lock = threading.Lock()
        self._params: dict = {}
        self._state = _VoiceState()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def active(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self) -> bool:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return True

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

    def set_params(self, params: dict) -> None:
        with self._lock:
            self._params = dict(params)

    def _warn(self, text: str) -> None:
        if self._reporter is not None:
            self._reporter.error(text)

    def _run(self) -> None:
        next_due = time.monotonic()
        seq = 0
        while not self._stop.is_set():
            with self._lock:
                params = dict(self._params)
            hd = _hd_voices(params)
            if hd is not None:
                left_v, right_v, _speaker = hd
                pcm = bt_render_pcm(left_v, right_v, _speaker, self._state)
            else:
                # 没有 HD 段（老固件 / 未接入）：发静音报文保持私有通路活跃。
                pcm = bytes(BT_PCM_BYTES)
            report = bt_build_report(pcm, seq)
            seq = (seq + 1) & 0xFF
            try:
                self._device.write(report)
            except OSError as exc:
                self._warn(f"DS5 蓝牙触觉流写回失败：{exc}")
                break
            next_due += BT_INTERVAL_S
            now = time.monotonic()
            if now - next_due > 0.1:
                # 落后超过一个容限（挂起/断连后追不上）：从当前时刻重新对表。
                next_due = now
            self._stop.wait(max(0.0, next_due - now))

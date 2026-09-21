"""DS5 音频触觉的 PC 侧合成（桥接路径）。

DualSense 连在 PC 上时音频接口被系统持有，触觉与喇叭改由 PC 侧送，两条通路共用同一份哑渲染：
- USB 直插：对 4ch 扬声器端点开 WASAPI 共享流（频道 3/4 是触觉音圈、1/2 是小喇叭）。
- 蓝牙：走私有报告（触觉 0x32 与成对音频+触觉的 0x36），每 10.67ms 一报由发送线程推送。

声部参数来自设备 FEEDBACK 帧的 57 字节 HD 版（固件已按布局行重整好，这里只做哑渲染；
老固件的 16 字节帧回落两带正弦），采样发声段铺扬声器之外同时折进两侧音圈。
用 RawOutputStream 而不是 OutputStream：后者的回调走 numpy 数组，而 numpy 原生扩展在本进程加载会卡死。
报文布局、包络门与让位语义见 docs/controller-ps.md。
"""
from __future__ import annotations

import math
import struct
import threading
import time
import zlib

#: 与固件 haptic_synth.c 同刻度：gain 255 的 int16 峰值（USB 承载与 0x36 的
#: 48kHz 喇叭块共用，与 DS5 布局行的 amp_peak 同值）。
AMP_PEAK_USB = 30000
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
#: 发声段音色的起音/收音时长（秒）：段边界硬切满幅/零幅会在小喇叭与音圈上
#: 听成咔哒（查找手柄页刺耳声的来源之一），包络在边沿内平滑过渡。
SPEAKER_ATTACK_S = 0.006
SPEAKER_RELEASE_S = 0.014
#: 音圈包络门的起音/收音时长（秒）：起音 1ms 爬满、收音 15ms 锁定最后发声的子帧淡出，
#: 避免块对齐硬切把短震动截没；与固件 haptic_synth 同一条曲线，门控只在有无增益的边沿发生。
COIL_ATTACK_S = 0.001
COIL_RELEASE_S = 0.015
#: 发声段音色的二次谐波比例与合成峰值回缩：给蜂鸣一点中空腔体，接近
#: Joy-Con 提示音的音色；谐波频率超过承载奈奎斯特频率时自动省去（蓝牙
#: 3kHz 承载 880/1175Hz 基频的二倍频已越界，混叠出不成调的杂音）。
SPEAKER_HARMONIC2 = 0.22
_SPEAKER_SHAPE = 1.0 / 1.09  # 基频+谐波的最坏相位叠加，压回峰值刻度
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
                 rate: int, peak: int, slice_samples: int,
                 gate: _CoilGate | None = None) -> list[int]:
    """一条时序子帧序列的渲染：首帧从子帧 0 起播整一切片，其后每
    slice_samples 帧切下一子帧（回绕），回绕长度就是该侧声明的子帧数——主机
    是 200Hz 的单子帧流（实抓 94% 的包只声明 1 个子帧），声明之外的槽位不
    占时间；固定按 3 槽轮播会把持续震动切成「5ms 有声 + 10ms 静默」的 66Hz
    断续（音圈的细腻手感退化成普通马达的粗糙震动），合成的强震段同理。
    相位跨块与跨子帧都连续（切子帧只换频率与增益，不重置相位）。

    gate 是音圈包络门（`_CoilGate`，跨块连续），不传按直渲处理（主机收震的
    下一块立刻全静）：带增益的参数到达且门开着（env 已落到 0）时游标与相位
    回零——新震动从自己的第一个子帧出去，不从上一段震动的游标位置续播；
    主机收震后锁定最后发声的子帧按 COIL_RELEASE_S 淡出——只占一块的短震动
    被拉到可感知的长度，结尾落点平滑且有界。门控只看参数级的有/无增益，
    子帧序列内部的静默切片不参与（时间轴不变）。"""
    count = side["count"]
    slots = count if 1 <= count <= KEY_MAX else KEY_MAX
    keys = side["keys"]
    lf_phase, hf_phase = phases
    idx, left = cursor
    if left == 0 or idx >= slots:
        idx = 0
        left = slice_samples
    out = [0] * frames
    if gate is None:
        for i in range(frames):
            if left == 0:
                idx = (idx + 1) % slots
                left = slice_samples
            left -= 1
            total = 0.0
            if idx < len(keys):
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
    active_key = None
    for k in range(min(len(keys), slots)):
        (lf, lg), (hf, hg) = keys[k]
        if lg or hg:
            active_key = keys[k]
            break
    if active_key is not None:
        gate.latch = active_key
    elif gate.env <= 0.0:
        gate.latch = None
    target = 1.0 if active_key is not None else 0.0
    if target == 1.0 and gate.env <= 0.0:
        idx = 0
        left = slice_samples
        lf_phase = 0.0
        hf_phase = 0.0
    rise = 1.0 / max(1.0, COIL_ATTACK_S * rate)
    fall = 1.0 / max(1.0, COIL_RELEASE_S * rate)
    env = gate.env
    for i in range(frames):
        if left == 0:
            idx = (idx + 1) % slots
            left = slice_samples
        left -= 1
        if target > env:
            env = min(1.0, env + rise)
        elif target < env:
            env = max(0.0, env - fall)
        total = 0.0
        if env > 0.0:
            if target == 1.0:
                key = keys[idx] if idx < len(keys) else None
            else:
                key = gate.latch
            if key is not None:
                (lf, lg), (hf, hg) = key
                if lg:
                    total += lg * peak / 255.0 * math.sin(lf_phase)
                    lf_phase = (lf_phase + _TAU * lf / rate) % _TAU
                if hg:
                    total += hg * peak / 255.0 * math.sin(hf_phase)
                    hf_phase = (hf_phase + _TAU * hf / rate) % _TAU
        out[i] = _clamp16(round(total * env))
    gate.env = env
    cursor[0], cursor[1] = idx, left
    phases[0], phases[1] = lf_phase, hf_phase
    return out


class _CoilGate:
    """单侧音圈的包络门（跨块连续）：env 是 0-1 的增益刻度（起音/收音在
    COIL_ATTACK_S / COIL_RELEASE_S 内推进），latch 是主机收震后收音尾锁定的
    最后发声子帧（env 落到 0 时清除）。"""

    __slots__ = ("env", "latch")

    def __init__(self) -> None:
        self.env = 0.0
        self.latch: tuple | None = None


class _VoiceState:
    """两侧振荡器相位 + 扬声器相位/包络 + 各侧子帧游标与音圈包络门（跨块连续）。

    speaker 是 3kHz 蓝牙承载（发声段折进音圈）的声部，speaker_beat 是 0x36
    喇叭块专用声部——两路采样率不同、相位与包络不能混用。gates 是左右音圈的
    包络门（`_CoilGate`），挂各自的子帧游标走。"""

    def __init__(self) -> None:
        self.key_phase = [[0.0, 0.0], [0.0, 0.0]]
        self.speaker = [0.0]
        self.speaker_env = 0.0
        self.speaker_beat = [0.0]
        self.speaker_beat_env = 0.0
        self.cursor = [[0, 0], [0, 0]]  # 每侧 [子帧序号, 距下次切换的样本数]
        self.gates = (_CoilGate(), _CoilGate())


def _render_speaker(tone: tuple, state: _VoiceState, frames: int, rate: int,
                    peak: int, beat: bool = False) -> list[int]:
    """发声段音色的哑渲染：基频 + 二次谐波，边沿触发起音/收音包络。
    state.speaker（3kHz，折进音圈）或 state.speaker_beat（喇叭块）是
    跨块连续的相位与包络（幅度刻度）——增益从 0 变非 0 时按起音时长爬升，
    归零时按收音时长衰落；谐波频率越过奈奎斯特界限就只出基频。"""
    freq, gain = tone
    full = gain * peak / 255.0
    step = _TAU * freq / rate if freq else 0.0
    harm = SPEAKER_HARMONIC2 if freq and 2 * freq < rate / 2 else 0.0
    attack = max(1.0, SPEAKER_ATTACK_S * rate)
    release = max(1.0, SPEAKER_RELEASE_S * rate)
    phase = state.speaker_beat[0] if beat else state.speaker[0]
    env = state.speaker_beat_env if beat else state.speaker_env
    out = [0] * frames
    for i in range(frames):
        if full > 0.0:
            env = min(full, env + full / attack)
        else:
            env = max(0.0, env - peak / release)
        if env <= 0.0:
            continue
        value = (math.sin(phase) + harm * math.sin(phase * 2)) * _SPEAKER_SHAPE
        out[i] = _clamp16(round(value * env))
        phase = (phase + step) % _TAU
    if beat:
        state.speaker_beat[0] = phase
        state.speaker_beat_env = env
    else:
        state.speaker[0] = phase
        state.speaker_env = env
    return out


def to_s8(value: int) -> int:
    """int16 刻度 → s8：饱和夹取（不回卷）。"""
    return max(-128, min(127, value))


def _side_active(side: dict) -> bool:
    """一侧的子帧序列里是否有非零增益的子帧。"""
    for key in tuple(side["keys"])[:max(0, side["count"])]:
        if key[0][1] or key[1][1]:
            return True
    return False


def _has_content(params: dict) -> bool:
    """参数里是否带要出的内容（任一侧音圈或发声段有增益）：发送线程的空闲
    唤醒按它判定。"""
    hd = params.get("hd")
    if hd is None:
        return False
    return _side_active(hd["l"]) or _side_active(hd["r"]) or bool(hd["speaker"][1])


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

    @property
    def engaged(self) -> bool:
        """音频流是否已经接到 HD 子帧（固件按布局行重整过时序子帧）。让位
        （`haptic audio on`）要等它置位：没接到内容的通路不驱动音圈，提前让位
        会把手柄留在「HID 震动已清零、音频也没有内容」的静默状态。"""
        with self._lock:
            return self._params.get("hd") is not None

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
                                frames, RATE, peak, slice_samples,
                                self._state.gates[0])
            right = _render_keys(right_v, self._state.key_phase[1], self._state.cursor[1],
                                 frames, RATE, peak, slice_samples,
                                 self._state.gates[1])
        else:
            left_v, right_v, speaker = _legacy_voices(params)
            left = _render_side(left_v, self._state.key_phase[0], frames, RATE, peak)
            right = _render_side(right_v, self._state.key_phase[1], frames, RATE, peak)
        sp = _render_speaker(speaker[0] if speaker else (0, 0),
                             self._state, frames, RATE, peak)
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


#: 蓝牙私有触觉流（SAxense 逆向）的报文形态：Report ID 0x32、共 142 字节
#: （Report ID + 137 字节报文体 + 4 字节 CRC32）。
BT_REPORT_LEN = 142
BT_REPORT_ID = 0x32
#: packet 0x12 承载 64 字节 PCM = 32 帧 × 2 声道 × 8-bit，3000Hz。
BT_PCM_BYTES = 64
BT_FRAMES = BT_PCM_BYTES // 2
BT_RATE = 3000
#: 发送节拍 = 一报承载的 PCM 时长：32 帧 / 3000Hz ≈ 10.67ms（约 94 报/秒）。
#: 0x32 与 0x36 两条承载共用它：节拍比块时长快就是过喂（控制器的 PCM 队列
#: 越积越多、震动被拉长），比块时长慢就是欠喂（短震动被截、起止不稳）。
BT_INTERVAL_S = BT_FRAMES / BT_RATE
#: CRC32 种子字节（PS 输出报告的 hidp 传输头，与 0x31 同一规则）。
BT_CRC_SEED = 0xA2


def bt_build_report(pcm: bytes, seq: int) -> bytes:
    """把 64 字节 PCM（32 帧交错双声道 s8）装进 0x32 私有报告。

    142 字节报文：包头 + packet 0x11 的配置与逐报递增序号 + packet 0x12 承载
    64 字节 PCM + 补零 + CRC32（种子 0xA2、小端，覆盖前 138 字节）。
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
                  state: _VoiceState, frames: int = BT_FRAMES) -> bytes:
    """子帧序列 → 一块 32 帧的触觉 PCM（交错左/右音圈 s8）：蓝牙上没有扬声器
    通道，发声段折进两侧音圈——与 USB 直插的音圈行为一致。两侧各过一道音圈
    包络门（起音/收音插值，见 `_render_keys`）。frames 是这一块的帧数：成对
    形态（0x39）一报两块，传 2 × BT_FRAMES。"""
    peak = AMP_PEAK_BT
    left = _render_keys(left_v, state.key_phase[0], state.cursor[0],
                        frames, BT_RATE, peak, _slice_samples(BT_RATE),
                        state.gates[0])
    right = _render_keys(right_v, state.key_phase[1], state.cursor[1],
                         frames, BT_RATE, peak, _slice_samples(BT_RATE),
                         state.gates[1])
    sp = _render_speaker(speaker[0] if speaker else (0, 0), state,
                         frames, BT_RATE, peak)
    out = bytearray(frames * 2)
    for i in range(frames):
        out[i * 2] = to_s8(left[i] + sp[i]) & 0xFF
        out[i * 2 + 1] = to_s8(right[i] + sp[i]) & 0xFF
    return bytes(out)


#: 蓝牙触觉+喇叭流（DS5Dongle/vds 逆向，DualSenseClient 同源）的报文形态：
#: Report ID 0x36、共 398 字节 = 报文头 + 配置包 + 63 字节状态块 +
#: 64 字节触觉 PCM（与 0x32 同格式）+ 200 字节 Opus 喇叭块 + 50 字节保留 +
#: 4 字节 CRC32。蓝牙描述符声明 0x36 为 397 字节数据，Windows 短写直达。
BT36_REPORT_LEN = 398
BT36_REPORT_ID = 0x36
#: 喇叭块：Opus CBR 160kbit → 每帧 200 字节；48kHz 声明下帧长 480 样本
#: （10ms 是 Opus 的帧长语法，不等于播放时长）。
BT36_SPEAKER_RATE = 48000
BT36_SPEAKER_FRAMES = 480
BT36_SPEAKER_BYTES = 200
#: 喇叭块的合成时钟：手柄按「一块对一拍」消耗 PCM，480 样本铺满整个 BT_INTERVAL_S 节拍
#: （约 45kHz 在播），因此一帧必须装下整拍内容——直接按节拍时钟合成 480 样本，
#: 等价于参考实现的重采样且没有欠喂误差。
BT36_SPEAKER_BEAT_RATE = round(BT36_SPEAKER_FRAMES / BT_INTERVAL_S)
#: 0x36 的状态块（vds kInitialSetStateData 经 set_audio_out_stream_active
#: 改写后的形态 + 16 字节保留零）：喇叭音量 100（PS5 缺省档）、音频控制字节
#: 的输出路径位段钉在手柄喇叭（0x30，初始 0x09 是耳机/自动——不路由的话
#: 喇叭块播进没插的耳机口，无声）、触觉走音频块；玩家灯与灯条字节清零——
#: 灯归 0x31 写回管。
BT36_STATE = bytes([
    0xFD, 0xF7, 0x00, 0x00, 0x7F, 100, 0x08, 0x39, 0x00, 0x0F,
] + [0] * 27 + [
    0x01, 0x07, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
]) + bytes(16)
#: 喇叭静默多少秒后从 0x36 退回 0x32：发声段之间的短停顿不切换承载。
#: 不做「采样按住期间保温」——那会让 0x36 在整个按住期间满速（100% 空口），
#: 挤占同频段的无线鼠标；冷启动延迟只在每个循环的第一声出现。
BT36_SPEAKER_TAIL_S = 0.3
#: 触觉静默多少秒后整条私有流停发：蓝牙无线电是 2.4GHz 公共介质，常驻空包
#: 会和同频段设备互相干扰。触觉块到手即播、没有需要保活的会话，空闲就一报不发。
BT_HAPTIC_TAIL_S = 0.15


def bt36_build_report(pcm: bytes, speaker: bytes, report_seq: int,
                      packet_seq: int) -> bytes:
    """装一份 0x36 报告：配置包 0x11 + 状态块 0x10 + 触觉 PCM 0x12 + Opus
    喇叭块 0x13；report_seq 是报告序号高半字节、packet_seq 是配置包内滚动
    序号（公开实现的同形布局：397 字节声明、[2]=0x91/[11]=0x90/[76]=0x92/
    [142]=0x93，[344:394] 保留零）。配置包第 4 字节取 0xFE（bit0 = 麦克风
    采集/双工模式，置位后手柄会把麦克风音频塞回 0x31 输入报告，被 Windows
    与 Steam 当成摇杆满偏——「手柄自己乱动」的幻输入来源；我们只要喇叭）。
    CRC32 与 0x31/0x32 同一条规则（种子 0xA2、覆盖前 394 字节、小端）。"""
    if len(pcm) != BT_PCM_BYTES:
        raise ValueError(f"pcm 需要 {BT_PCM_BYTES} 字节，收到 {len(pcm)}")
    if len(speaker) != BT36_SPEAKER_BYTES:
        raise ValueError(f"speaker 需要 {BT36_SPEAKER_BYTES} 字节，收到 {len(speaker)}")
    report = bytearray(BT36_REPORT_LEN)
    report[0] = BT36_REPORT_ID
    report[1] = (report_seq & 0xF) << 4
    report[2] = 0x11 | 0x80
    report[3] = 7
    report[4] = 0xFE  # 音频段全开、不开麦克风采集（0xFF 会打开双工幻输入）
    report[5:10] = bytes([64] * 5)  # 音频缓冲长度
    report[10] = packet_seq & 0xFF
    report[11] = 0x10 | 0x80  # 状态块
    report[12] = len(BT36_STATE)
    report[13:13 + len(BT36_STATE)] = BT36_STATE
    report[76] = 0x12 | 0x80  # 触觉 PCM
    report[77] = BT_PCM_BYTES
    report[78:78 + BT_PCM_BYTES] = pcm
    report[142] = 0x13 | 0x80  # 目标 = 手柄喇叭（0x16 是耳机）
    report[143] = BT36_SPEAKER_BYTES
    report[144:144 + BT36_SPEAKER_BYTES] = speaker
    crc = zlib.crc32(bytes([BT_CRC_SEED]) + bytes(report[:BT36_REPORT_LEN - 4]))
    report[BT36_REPORT_LEN - 4:BT36_REPORT_LEN] = struct.pack("<I", crc)
    return bytes(report)


def render_speaker_beat(tone: tuple, state: _VoiceState) -> bytes:
    """发声段音色 → 一块整节拍（480 样本 × 立体声 int16 小端）：0x36 的喇叭块
    输入。按节拍时钟（BT36_SPEAKER_BEAT_RATE）合成，一块装下整拍的实时内容；
    相位与包络挂在 state.speaker_beat 上，与 3kHz 音圈通路互不干扰。"""
    sp = _render_speaker(tone, state, BT36_SPEAKER_FRAMES,
                         BT36_SPEAKER_BEAT_RATE, AMP_PEAK_USB, beat=True)
    out = bytearray(BT36_SPEAKER_FRAMES * 4)
    for i, v in enumerate(sp):
        struct.pack_into("<hh", out, i * 4, v, v)
    return bytes(out)


#: 蓝牙「成对」音频+触觉流（547 字节）：一报带 2 个触觉块与 2 个 Opus 喇叭帧、
#: 节拍 21.33ms，多带的那一块是链路抖动的水垫；没有状态块，
#: 音频路由由会话开始时那一份 0x31 预置保持。
BT39_REPORT_LEN = 547
BT39_REPORT_ID = 0x39
BT39_HAPTIC_BYTES = BT_PCM_BYTES * 2
BT39_SPEAKER_BYTES = BT36_SPEAKER_BYTES * 2
BT39_INTERVAL_S = BT_INTERVAL_S * 2.0


def bt39_build_report(coil: bytes, speaker: bytes, report_seq: int,
                      packet_seq: int) -> bytes:
    """装一份 0x39 成对报告：配置包 0x11（长度 6）+ 触觉包 0x12（2 块 64 字节
    PCM）+ 喇叭包 0x13（2 个 200 字节 Opus 帧）+ 尾部 CRC32（种子 0xA2、覆盖
    前 543 字节，与 0x31/0x32/0x36 同一条规则）。偏移对齐 DS5Dongle 的
    audio_bt_task：[2]=0x91、[10]=0x92、[11]=64、[12:140] 触觉、[140]=0x93、
    [141]=200、[142:542] 喇叭。"""
    if len(coil) != BT39_HAPTIC_BYTES:
        raise ValueError(f"触觉 PCM 需要 {BT39_HAPTIC_BYTES} 字节，收到 {len(coil)}")
    if len(speaker) != BT39_SPEAKER_BYTES:
        raise ValueError(f"喇叭块需要 {BT39_SPEAKER_BYTES} 字节，收到 {len(speaker)}")
    report = bytearray(BT39_REPORT_LEN)
    report[0] = BT39_REPORT_ID
    report[1] = (report_seq & 0xF) << 4
    report[2] = 0x11 | 0x80
    report[3] = 6
    report[4] = 0xFE  # 音频段全开、不开麦克风采集（0xFF 会打开双工幻输入）
    report[5:9] = bytes([64] * 4)  # 音频缓冲长度
    report[9] = packet_seq & 0xFF
    report[10] = 0x12 | 0x80
    report[11] = BT_PCM_BYTES
    report[12:12 + BT39_HAPTIC_BYTES] = coil
    report[140] = 0x13 | 0x80
    report[141] = BT36_SPEAKER_BYTES
    report[142:142 + BT39_SPEAKER_BYTES] = speaker
    crc = zlib.crc32(bytes([BT_CRC_SEED]) + bytes(report[:BT39_REPORT_LEN - 4]))
    report[BT39_REPORT_LEN - 4:BT39_REPORT_LEN] = struct.pack("<I", crc)
    return bytes(report)


def render_speaker_pair(tone: tuple, state: _VoiceState) -> bytes:
    """一报两块喇叭内容：连着合成 2 个 480 样本的帧（21.33ms），相位与包络跨帧
    连续（第二帧接着第一帧走），按 1920 字节一帧切开、交给同一个 48kHz 声明值
    的编码器逐帧编码（编码器一次只吃一帧）。"""
    first = render_speaker_beat(tone, state)
    second = render_speaker_beat(tone, state)
    return first + second

    """发声段音色 → 一块整节拍（480 样本 × 立体声 int16 小端）：0x36 的喇叭块
    输入。按节拍时钟（BT36_SPEAKER_BEAT_RATE）合成，一块装下整拍的实时内容；
    相位与包络挂在 state.speaker_beat 上，与 3kHz 音圈通路互不干扰。"""
    sp = _render_speaker(tone, state, BT36_SPEAKER_FRAMES,
                         BT36_SPEAKER_BEAT_RATE, AMP_PEAK_USB, beat=True)
    out = bytearray(BT36_SPEAKER_FRAMES * 4)
    for i, v in enumerate(sp):
        struct.pack_into("<hh", out, i * 4, v, v)
    return bytes(out)


class Bt36OpusEncoder:
    """48kHz 立体声 10ms CBR Opus 编码器（PyAV/libopus，vds SpeakerEncoder
    同参数：VBR 关、complexity 0、码率 160kbit）。块尾不足 200 字节时补零，
    超长截断。构造失败（PyAV 缺失/无 libopus）由调用方按回落处理。"""

    def __init__(self) -> None:
        import av

        self._av = av
        ctx = av.CodecContext.create("libopus", "w")
        ctx.sample_rate = BT36_SPEAKER_RATE
        ctx.layout = "stereo"
        ctx.format = "s16"
        ctx.bit_rate = BT36_SPEAKER_BYTES * 8 * 100
        ctx.options = {"frame_duration": "10", "vbr": "off",
                       "compression_level": "0"}
        ctx.open()
        self._ctx = ctx

    def encode(self, pcm: bytes) -> bytes:
        if len(pcm) != BT36_SPEAKER_FRAMES * 4:
            raise ValueError(f"喇叭 PCM 需要 {BT36_SPEAKER_FRAMES * 4} 字节，"
                             f"收到 {len(pcm)}")
        frame = self._av.AudioFrame(format="s16", layout="stereo",
                                    samples=BT36_SPEAKER_FRAMES)
        frame.sample_rate = BT36_SPEAKER_RATE
        frame.planes[0].update(pcm)
        chunk = bytearray()
        for packet in self._ctx.encode(frame):
            chunk += bytes(packet)
        # 帧长与块长一致（10ms）时 libopus 逐帧出包、无延迟队列，不做 flush
        # （drain 会把编码器打进收尾态）。
        return chunk.ljust(BT36_SPEAKER_BYTES, b"\x00")[:BT36_SPEAKER_BYTES]


class Ds5HapticsBt:
    """蓝牙连接的 DualSense 私有触觉流：有内容时按 10.67ms 节拍把子帧序列渲染
    成 0x32 报告（发声段折进两侧音圈——蓝牙没有扬声器通道，音圈是它唯一的载体）；
    给了 speaker_encoder（Bt36OpusEncoder）时发声段改走 0x36 报文：同样按一报里
    触觉 PCM 的时长（10.67ms）出报、触觉块不折喇叭，由真正的手柄喇叭出声。
    空闲整流停发——常驻空包会和同频段设备互相干扰，触觉块到手即播、无会话可保活；
    停发期间 set_params 带新内容时即时唤醒发送线程（短震动的第一拍不等 20ms
    兜底轮询才被看见）。发送线程独立于会话主循环（蓝牙 HID 写回慢，不能占桥接
    热路径）。写回被拒时经 on_error 通知会话（回落 HID 震动写回），蓝牙不至于整路静默。

    发送时刻钉在固定网格上（`_run`）：渲染与写回的耗时不计入周期，落后超过
    一拍就重新对表、不连发追赶；空闲唤醒也从当前时刻重新对表。clock 是取时刻
    的入口（默认 time.monotonic），主机端用例用假时钟把整条节拍瞬间跑完。"""

    LABEL = "DS5 蓝牙触觉流已启用（0x32 私有报文，HID 震动让位）"
    LABEL_36 = "DS5 蓝牙触觉流已启用（0x36 HD 触觉 + 手柄喇叭，HID 震动让位）"

    def __init__(self, device, reporter=None, on_error=None,
                 speaker_encoder=None, clock=time.monotonic, pair=False) -> None:
        self._device = device
        self._reporter = reporter
        self._on_error = on_error
        self._speaker_encoder = speaker_encoder
        #: 成对形态（0x39）：一报 2 块触觉 + 2 帧喇叭、节拍 21.33ms。链路抖动
        #: 的容差翻倍（单块形态下一拍迟到 10.67ms 就断音），报数减半也少一半
        #: 链路开销；需要编码器（喇叭块随报固定带 2 帧）。
        self._pair = bool(pair) and speaker_encoder is not None
        self._interval = BT39_INTERVAL_S if self._pair else BT_INTERVAL_S
        self._clock = clock
        self._lock = threading.Lock()
        self._params: dict = {}
        self._state = _VoiceState()
        self._state_beat = _VoiceState()  # 0x36 喇叭块专用声部（每拍一块）
        self._last_speaker_at = 0.0
        self._last_coil_at = 0.0
        self._stats = {"writes": 0, "write_ms_total": 0.0,
                       "write_ms_max": 0.0, "late": 0,
                       "beat_s": BT_INTERVAL_S,
                       "short": 0,
                       "short_recovered": 0,
                       "first_at": 0.0, "last_at": 0.0}
        #: 起震延迟统计（内容到达 → 首报写出）：空闲整流停发期间靠 set_params
        #: 唤醒，正在按节拍推流时则要等下一个节拍点，这一段延迟是「不及时」的
        #: 主要来源，统计它才能判断要不要动节拍。
        self._pending_since: float | None = None
        self._onset_ms_total = 0.0
        self._onset_ms_max = 0.0
        self._onsets = 0
        self._engaged = False
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def active(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    @property
    def speaker_active(self) -> bool:
        return self._speaker_encoder is not None

    @property
    def label(self) -> str:
        return self.LABEL_36 if self._speaker_encoder is not None else self.LABEL

    def stats(self) -> str:
        """写回链路统计（份数、平均/最大单次写回耗时、超节拍份数、实际节拍）：
        实际节拍要从头到尾贴着当前形态的节拍（单块 10.67ms / 成对 21.33ms）
        ——比它长说明写回或渲染吃掉了周期（触觉 PCM 供不上控制器的 3kHz 消耗，
        短震动被拉长），比它短说明在连发追赶（控制器的 PCM 队列被一次塞满）。"""
        s = self._stats
        if s["writes"] == 0:
            return "写回统计：无写回"
        avg = s["write_ms_total"] / s["writes"]
        span_ms = (s["last_at"] - s["first_at"]) * 1000.0
        beat = f"{span_ms / (s['writes'] - 1):.2f}ms" if s["writes"] > 1 else "—"
        onset = (f"，起震延迟 平均 {self._onset_ms_total / self._onsets:.1f}ms/"
                 f"最大 {self._onset_ms_max:.1f}ms（{self._onsets} 次）"
                 if self._onsets else "")
        # 短写：hidapi 用返回值报「实际交给驱动的字节数」，写满才算这一拍真的
        # 出去了——只捕异常看不出静默丢失（蓝牙输出队列满时正是这种表现）。
        short = (f"，短写 {s['short']} 份（重发成功 {s['short_recovered']}）"
                 if s["short"] else "")
        return (f"写回统计：{s['writes']} 份，平均 {avg:.1f}ms/份，"
                f"最大 {s['write_ms_max']:.1f}ms，超节拍 {s['late']} 份，"
                f"实际节拍 {beat}（目标 {s['beat_s'] * 1000.0:.2f}ms）{onset}{short}")

    def start(self) -> bool:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return True

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

    def set_params(self, params: dict) -> None:
        with self._lock:
            self._params = dict(params)
        # 新内容到达即时唤醒空闲的发送线程：整流停发期间的第一拍震动不等
        # 20ms 兜底轮询才被看见——短震动的启动延迟少掉一个轮询拍。
        if _has_content(params):
            if self._pending_since is None:
                self._pending_since = self._clock()
            self._wake.set()

    @property
    def engaged(self) -> bool:
        """私有流是否已经接到 HD 子帧（真的在驱动音圈）。让位（`haptic audio
        on`）要等它置位：没接到内容的流一报不发，提前让位会把手柄留在「HID
        震动已清零、音频也没有内容」的静默状态。"""
        return self._engaged

    def _warn(self, text: str) -> None:
        if self._reporter is not None:
            self._reporter.error(text)

    def _note(self, text: str) -> None:
        """一条普通提示（仅在 reporter 支持 line 时输出）：启用行只说明流对象开起来了，
        收到 HD 子帧才是真的在驱动触觉音圈。"""
        line = getattr(self._reporter, "line", None)
        if line is not None:
            line(text)

    @staticmethod
    def _coil_active(left_v, right_v) -> bool:
        """当前拍音圈是否有内容（任一子帧增益非零）。"""
        return left_v is not None and (_side_active(left_v) or _side_active(right_v))

    @staticmethod
    def _describe(side: dict) -> str:
        """子帧序列的可读描述（诊断用）：每子帧「低频/增益 + 高频/增益」，没有内容给「静默」。
        这一行给出固件下发的档位——增益仍在 5 上下说明布局行的放大没进固件，20 上下才是标定值。"""
        keys = tuple(side["keys"])[:max(0, side["count"])]
        if not keys:
            return "静默"
        return " ".join(f"{lf}Hz/{lg}+{hf}Hz/{hg}" for (lf, lg), (hf, hg) in keys)

    def _wait_until_due(self, due: float) -> bool:
        """等到下一拍（到点就是等 0）：返回 True 表示该收尾了（停止位）。"""
        return self._stop.wait(max(0.0, due - self._clock()))

    def _wait_for_content(self, timeout: float) -> None:
        """空闲等待：内容到达（`set_params` 唤醒）或兜底轮询超时后返回。"""
        if self._wake.wait(timeout):
            self._wake.clear()

    def _run(self) -> None:
        clock = self._clock
        next_due = clock()
        seq = 0
        packet_seq = 0
        while not self._stop.is_set():
            # 到点才产报：等待排在渲染之前，周期只由网格决定——渲染、编码与
            # 写回的耗时都落在节拍内，写回慢也只让这一拍晚一点发出。
            if self._wait_until_due(next_due):
                break
            with self._lock:
                params = dict(self._params)
            hd = _hd_voices(params)
            if hd is not None:
                left_v, right_v, speaker = hd
            else:
                # 没有 HD 段（老固件 / 未接入）：等同于空闲，不发报。
                left_v = right_v = None
                speaker = ()
            if hd is not None and not self._engaged:
                # 开关打开与线程启动只说明通路就绪，收到 HD 子帧才开始驱动音圈。
                self._engaged = True
                carrier = "0x36 HD + 手柄喇叭" if self._speaker_encoder else "0x32 音圈"
                self._note(f"蓝牙触觉流接到主机的 HD 子帧，开始驱动触觉音圈（{carrier}）："
                           f"左 {self._describe(left_v)} / 右 {self._describe(right_v)}")
            now = clock()
            tone = speaker[0] if speaker else (0, 0)
            if tone[1]:
                self._last_speaker_at = now
            if self._coil_active(left_v, right_v):
                self._last_coil_at = now
            # 0x36 只在喇叭真有内容（含收音尾）时上；触觉走 0x32（发声段
            # 折进音圈兜底）；两条静默超尾长就整流停发——常驻空包会和同频段
            # 设备互相干扰，触觉块到手即播、没有需要保活的会话。
            use_36 = (self._speaker_encoder is not None and
                      now - self._last_speaker_at < BT36_SPEAKER_TAIL_S)
            haptic_recent = (now - self._last_coil_at < BT_HAPTIC_TAIL_S or
                             now - self._last_speaker_at < BT_HAPTIC_TAIL_S)
            if use_36:
                if self._pair:
                    # 成对形态（0x39）：一报 2 块触觉（128 字节 PCM）+ 2 帧
                    # 喇叭，节拍 21.33ms——多带的那一块是链路抖动的水垫（单块
                    # 形态下一拍迟到 10.67ms 就断音），报数减半也少一半开销。
                    coil = (bytes(BT_PCM_BYTES * 2) if left_v is None else
                            bt_render_pcm(left_v, right_v, (), self._state,
                                          frames=BT_FRAMES * 2))
                    # 编码器一次只吃一帧（480 样本），成对形态逐帧编码后拼成
                    # 400 字节：两块喇叭内容各占 200 字节，与声明长度一致。
                    pair_pcm = render_speaker_pair(tone, self._state_beat)
                    frame_bytes = BT36_SPEAKER_FRAMES * 4
                    speaker_block = (
                        self._speaker_encoder.encode(pair_pcm[:frame_bytes]) +
                        self._speaker_encoder.encode(pair_pcm[frame_bytes:]))
                    report = bt39_build_report(coil, speaker_block,
                                               report_seq=seq,
                                               packet_seq=packet_seq)
                else:
                    if left_v is None:
                        coil = bytes(BT_PCM_BYTES)
                    else:
                        coil = bt_render_pcm(left_v, right_v, (), self._state)
                    speaker_block = self._speaker_encoder.encode(
                        render_speaker_beat(tone, self._state_beat))
                    report = bt36_build_report(coil, speaker_block,
                                               report_seq=seq,
                                               packet_seq=packet_seq)
                seq = (seq + 1) & 0xF
                packet_seq = (packet_seq + 1) & 0xFF
                beat_s = self._interval if self._pair else BT_INTERVAL_S
            elif haptic_recent:
                if left_v is None:
                    pcm = bytes(BT_PCM_BYTES)
                else:
                    pcm = bt_render_pcm(left_v, right_v, speaker, self._state)
                report = bt_build_report(pcm, seq)
                seq = (seq + 1) & 0xFF
                beat_s = BT_INTERVAL_S
            else:
                # 空闲：一报不发，等 set_params 的内容唤醒或 20ms 兜底轮询
                # （新震动的第一拍不等下一个轮询拍才被看见）；醒来把节拍网格
                # 挪到当前时刻：空闲时长不定，续用空闲前的网格会连着补几拍，
                # 把控制器的 PCM 队列一次塞满。
                self._wait_for_content(0.02)
                next_due = clock()
                continue
            try:
                t0 = clock()
                written = self._device.write(report)
                done = clock()
                write_ms = (done - t0) * 1000.0
                s = self._stats
                # Windows 的 hidapi 会把短于描述符声明长度的写回补齐到
                # OutputReportByteLength（DS5 蓝牙集合声明 547）再交驱动，返回值
                # 因此常比报告本身长——只有真的少交（< 报告长度）才是这一拍
                # 没进队列。把补齐当短写会每拍重发一次，音圈 PCM 被双倍喂进队列。
                if isinstance(written, int) and written < len(report):
                    s["short"] += 1
                    if s["short"] == 1:
                        self._note(f"蓝牙触觉流首份短写：{written}/{len(report)} 字节"
                                   "——输出队列没收下这一拍，手柄这段收不到")
                    # 短写是「驱动没把这一份收进队列」：立刻原样重发一次，能把
                    # 队列瞬时满丢掉的拍救回来（内容仍是这一段波形，相位不跳）。
                    try:
                        retry = self._device.write(report)
                    except OSError:
                        retry = -1
                    if isinstance(retry, int) and retry >= len(report):
                        s["short_recovered"] += 1
                if s["writes"] == 0:
                    s["first_at"] = t0
                s["writes"] += 1
                s["last_at"] = done
                s["write_ms_total"] += write_ms
                s["write_ms_max"] = max(s["write_ms_max"], write_ms)
                s["beat_s"] = beat_s
                if write_ms > beat_s * 1000.0:
                    s["late"] += 1
                # 起震延迟：内容到达（set_params 置位）到首报写出之间的时间。
                if self._pending_since is not None:
                    onset_ms = max(0.0, (done - self._pending_since) * 1000.0)
                    self._onset_ms_total += onset_ms
                    self._onset_ms_max = max(self._onset_ms_max, onset_ms)
                    self._onsets += 1
                    self._pending_since = None
            except OSError as exc:
                self._warn(f"DS5 蓝牙触觉流写回失败：{exc}")
                if self._on_error is not None:
                    self._on_error(exc)
                break
            next_due += beat_s
            after = clock()
            if after - next_due > beat_s:
                # 落后超过一拍（系统挂起、写回卡住）：网格挪到当前时刻，
                # 不做连发追赶。
                next_due = after

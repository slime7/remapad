"""DS5 音频触觉的 PC 侧合成（桥接路径）。

DualSense 连在 PC 上时有两条投递通路，共用同一份哑渲染：
- USB 直插：音频接口由 Windows 持有（usbaudio.sys），对它的 4ch 扬声器端点
  开 WASAPI 共享流——频道 3/4（RL/RR）直连左右触觉音圈，频道 1/2 是手柄
  小喇叭（采样提示音的发声段，没有真正的声音时恒零）。
- 蓝牙：HID 之外没有音频接口，触觉走 SAxense 逆向的私有报告 0x32（142 字节
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
    """两侧振荡器相位 + 扬声器相位/包络 + 各侧子帧游标（跨块连续）。

    speaker 是 3kHz 蓝牙承载（发声段折进音圈）的声部，speaker48 是 0x36 的
    48kHz 喇叭块专用声部——两路采样率不同、相位与包络不能混用。"""

    def __init__(self) -> None:
        self.key_phase = [[0.0, 0.0], [0.0, 0.0]]
        self.speaker = [0.0]
        self.speaker_env = 0.0
        self.speaker48 = [0.0]
        self.speaker48_env = 0.0
        self.cursor = [[0, 0], [0, 0]]  # 每侧 [子帧序号, 距下次切换的样本数]


def _render_speaker(tone: tuple, state: _VoiceState, frames: int, rate: int,
                    peak: int, hi: bool = False) -> list[int]:
    """发声段音色的哑渲染：基频 + 二次谐波，边沿触发起音/收音包络。
    state.speaker（3kHz，折进音圈）或 state.speaker48（48kHz 喇叭块）是
    跨块连续的相位与包络（幅度刻度）——增益从 0 变非 0 时按起音时长爬升，
    归零时按收音时长衰落；谐波频率越过奈奎斯特界限就只出基频。"""
    freq, gain = tone
    full = gain * peak / 255.0
    step = _TAU * freq / rate if freq else 0.0
    harm = SPEAKER_HARMONIC2 if freq and 2 * freq < rate / 2 else 0.0
    attack = max(1.0, SPEAKER_ATTACK_S * rate)
    release = max(1.0, SPEAKER_RELEASE_S * rate)
    phase = state.speaker48[0] if hi else state.speaker[0]
    env = state.speaker48_env if hi else state.speaker_env
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
    if hi:
        state.speaker48[0] = phase
        state.speaker48_env = env
    else:
        state.speaker[0] = phase
        state.speaker_env = env
    return out


def to_s8(value: int) -> int:
    """int16 刻度 → s8：饱和夹取（不回卷）。"""
    return max(-128, min(127, value))


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
#: 发送节拍：32 帧 / 3000Hz ≈ 10.67ms（约 94 报/秒）。
BT_INTERVAL_S = BT_FRAMES / BT_RATE
#: CRC32 种子字节（PS 输出报告的 hidp 传输头，与 0x31 同一规则）。
BT_CRC_SEED = 0xA2


def bt_build_report(pcm: bytes, seq: int) -> bytes:
    """把 64 字节 PCM（32 帧交错双声道 s8）装进 0x32 私有报告。

    布局（SAxense.c，与真机互通的公开实现）：共 142 字节，[0]=0x32、
    [1]=tag/seq 字节保持 0（递增序号在 packet 0x11 内）、[2]=0x91（packet
    0x11 + sized 位）、[3]=长度 7、[4:11] = 配置 `FE 00 00 00 00 FF <seq>`
    （序号在 [10]，逐报递增）、[11]=0x92（packet 0x12 + sized）、[12]=0x40、
    [13:77] = PCM、其后补零到 138 字节，尾部 4 字节是 CRC32（种子 0xA2 先过
    一遍、小端，覆盖 [0:138] 共 138 字节）。
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
    sp = _render_speaker(speaker[0] if speaker else (0, 0), state,
                         BT_FRAMES, BT_RATE, peak)
    out = bytearray(BT_PCM_BYTES)
    for i in range(BT_FRAMES):
        out[i * 2] = to_s8(left[i] + sp[i]) & 0xFF
        out[i * 2 + 1] = to_s8(right[i] + sp[i]) & 0xFF
    return bytes(out)


#: 蓝牙触觉+喇叭流（DS5Dongle/vds 逆向，DualSenseClient 同源）的报文形态：
#: Report ID 0x36、共 398 字节 = 报文头 + 配置包 + 63 字节状态块 +
#: 64 字节触觉 PCM（与 0x32 同格式）+ 200 字节 Opus 喇叭块 + 50 字节保留 +
#: 4 字节 CRC32。蓝牙描述符声明 0x36 为 397 字节数据，Windows 短写直达。
BT36_REPORT_LEN = 398
BT36_REPORT_ID = 0x36
#: 喇叭块：48kHz 立体声 10ms（480 帧），Opus CBR 码率 = 200B × 8 × 100 包/s。
BT36_SPEAKER_RATE = 48000
BT36_SPEAKER_FRAMES = 480
BT36_SPEAKER_BYTES = 200
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
#: 发送节拍：喇叭块 10ms 一报（触觉块 64B = 32 帧 ≈ 10.67ms，按喇叭节拍走，
#: vds 的取法——按触觉时长对表会让喇叭周期性欠喂）。
BT36_INTERVAL_S = 0.010
#: 喇叭静默多少秒后从 0x36 退回 0x32：发声段之间的短停顿不切换承载。
#: 不做「采样按住期间保温」——那会让 0x36 在整个按住期间满速（100% 空口），
#: 同频段无线鼠标全程被骚扰（实机复测）；空口让给鼠标，冷启动延迟只在
#: 每个循环的第一声出现且被拥塞消除的大头抵消。
BT36_SPEAKER_TAIL_S = 0.3
#: 触觉静默多少秒后整条私有流停发：蓝牙无线电是 2.4GHz 公共介质，常驻空包
#: 会和同频段的无线鼠标互相干扰（实机：鼠标卡、触控板幻手势弹 OSK/开始
#: 菜单）。触觉块到手即播、没有需要保活的会话，空闲就一报不发。
BT_HAPTIC_TAIL_S = 0.15


def bt36_build_report(pcm: bytes, speaker: bytes, report_seq: int,
                      packet_seq: int) -> bytes:
    """装一份 0x36 报告：配置包（关麦克风、开喇叭）+ 状态块 + 触觉 PCM +
    Opus 喇叭块；report_seq 是报告序号高半字节、packet_seq 是配置包内滚动
    序号。CRC32 与 0x31/0x32 同一条规则（种子 0xA2、覆盖除 CRC 外全部字节）。"""
    if len(pcm) != BT_PCM_BYTES:
        raise ValueError(f"pcm 需要 {BT_PCM_BYTES} 字节，收到 {len(pcm)}")
    if len(speaker) != BT36_SPEAKER_BYTES:
        raise ValueError(f"speaker 需要 {BT36_SPEAKER_BYTES} 字节，收到 {len(speaker)}")
    report = bytearray(BT36_REPORT_LEN)
    report[0] = BT36_REPORT_ID
    report[1] = (report_seq & 0xF) << 4
    report[2] = 0x11 | 0x80
    report[3] = 7
    report[4] = 0xFF  # 音频段全开（vds 实发值；0xFE 是关麦克风的变体）
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


def render_speaker_48k(tone: tuple, state: _VoiceState) -> bytes:
    """发声段音色 → 一块 10ms 的 48kHz 立体声 int16（小端）：0x36 的喇叭块
    输入。相位与包络挂在 state.speaker48 上，与 3kHz 音圈通路互不干扰。"""
    sp = _render_speaker(tone, state, BT36_SPEAKER_FRAMES, BT36_SPEAKER_RATE,
                         AMP_PEAK_USB, hi=True)
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
    成 0x32 报告（142 字节 SAxense 形态，发声段折进两侧音圈——蓝牙没有扬声器
    通道，音圈是它唯一的载体）；给了 speaker_encoder（Bt36OpusEncoder）时发声段
    改走 0x36 报文（398 字节，vds 形态）：10ms 节拍、触觉块不折喇叭，由真正的
    手柄喇叭出声。空闲整流停发——蓝牙无线电是 2.4GHz 公共介质，常驻空包会和
    同频段设备互相干扰（实机：无线鼠标卡顿、触控板幻手势），触觉块到手即播、
    无会话可保活；发送线程独立于会话主循环（蓝牙 HID 写回慢，不能占桥接热
    路径）。写回被拒时经 on_error 通知会话（回落 HID 震动写回），蓝牙不至于
    整路静默。"""

    LABEL = "DS5 蓝牙触觉流已启用（0x32 私有报文，HID 震动让位）"
    LABEL_36 = "DS5 蓝牙触觉流已启用（0x36 HD 触觉 + 手柄喇叭，HID 震动让位）"

    def __init__(self, device, reporter=None, on_error=None,
                 speaker_encoder=None) -> None:
        self._device = device
        self._reporter = reporter
        self._on_error = on_error
        self._speaker_encoder = speaker_encoder
        self._lock = threading.Lock()
        self._params: dict = {}
        self._state = _VoiceState()
        self._state48 = _VoiceState()  # 0x36 喇叭块的 48kHz 声部
        self._last_speaker_at = 0.0
        self._last_coil_at = 0.0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def active(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    @property
    def speaker_active(self) -> bool:
        return self._speaker_encoder is not None

    @property
    def _interval_s(self) -> float:
        return BT36_INTERVAL_S if self._speaker_encoder is not None else BT_INTERVAL_S

    @property
    def label(self) -> str:
        return self.LABEL_36 if self._speaker_encoder is not None else self.LABEL

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

    @staticmethod
    def _coil_active(left_v, right_v) -> bool:
        """当前拍音圈是否有内容（任一子帧增益非零）。"""
        if left_v is None:
            return False
        for side in (left_v, right_v):
            for key in tuple(side["keys"])[:max(0, side["count"])]:
                if key[0][1] or key[1][1]:
                    return True
        return False

    def _run(self) -> None:
        next_due = time.monotonic()
        seq = 0
        packet_seq = 0
        while not self._stop.is_set():
            with self._lock:
                params = dict(self._params)
            hd = _hd_voices(params)
            if hd is not None:
                left_v, right_v, speaker = hd
            else:
                # 没有 HD 段（老固件 / 未接入）：等同于空闲，不发报。
                left_v = right_v = None
                speaker = ()
            now = time.monotonic()
            tone = speaker[0] if speaker else (0, 0)
            if tone[1]:
                self._last_speaker_at = now
            if self._coil_active(left_v, right_v):
                self._last_coil_at = now
            # 0x36 只在喇叭真有内容（含收音尾）时上；触觉走 0x32（发声段
            # 折进音圈兜底）；两条静默超尾长就整流停发——蓝牙无线电是公共
            # 介质，常驻空包会和同频段设备互相干扰（实机：2.4GHz 无线鼠标
            # 卡顿、触控板幻手势弹 OSK/开始菜单）。触觉块到手即播，没有
            # 需要保活的会话，也不做采样按住期间的满速保温。
            use_36 = (self._speaker_encoder is not None and
                      now - self._last_speaker_at < BT36_SPEAKER_TAIL_S)
            haptic_recent = (now - self._last_coil_at < BT_HAPTIC_TAIL_S or
                             now - self._last_speaker_at < BT_HAPTIC_TAIL_S)
            if use_36:
                if left_v is None:
                    coil = bytes(BT_PCM_BYTES)
                else:
                    coil = bt_render_pcm(left_v, right_v, (), self._state)
                speaker_block = self._speaker_encoder.encode(
                    render_speaker_48k(tone, self._state48))
                report = bt36_build_report(coil, speaker_block,
                                           report_seq=seq,
                                           packet_seq=packet_seq)
                seq = (seq + 1) & 0xF
                packet_seq = (packet_seq + 1) & 0xFF
                interval = BT36_INTERVAL_S
            elif haptic_recent:
                if left_v is None:
                    pcm = bytes(BT_PCM_BYTES)
                else:
                    pcm = bt_render_pcm(left_v, right_v, speaker, self._state)
                report = bt_build_report(pcm, seq)
                seq = (seq + 1) & 0xFF
                interval = BT_INTERVAL_S
            else:
                # 空闲：一报不发，等下一拍内容（next_due 由苏醒后的重对表
                # 兜底，不在这里推进）。
                self._stop.wait(0.02)
                continue
            try:
                self._device.write(report)
            except OSError as exc:
                self._warn(f"DS5 蓝牙触觉流写回失败：{exc}")
                if self._on_error is not None:
                    self._on_error(exc)
                break
            next_due += interval
            if now - next_due > 0.1:
                # 落后超过一个容限（挂起/断连后追不上）：从当前时刻重新对表。
                next_due = now
            self._stop.wait(max(0.0, next_due - now))

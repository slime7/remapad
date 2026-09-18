"""DS5 音频触觉的 PC 侧合成（桥接路径）。

DualSense 插在 PC 上时音频接口由 Windows 持有（usbaudio.sys），触觉波形只能
从 PC 侧送：对它的 4ch 扬声器端点开 WASAPI 共享流，前两路（扬声器）恒零，
后两路（RL/RR，直连左右触觉音圈）放合成正弦——PS5 驱动触觉用的正是这对
通道。参数来自设备的 FEEDBACK 帧：两带振幅与频率落地值都在固件里算好
（haptic_synth 同一套刻度），这里只做哑渲染；端点开不起来就回落 HID 震动。
2026-09-18 实测：共享流 4ch 独立可控、扬声器不漏音。

用 RawOutputStream 而不是 OutputStream：后者的回调走 numpy 数组，而 numpy
的原生扩展在会话进程里加载会卡死（cffi/PortAudio 都正常，仅 numpy 如此，
faulthandler 抓栈定位）；raw 模式回调收字节缓冲，struct 直写 int16，
与固件的 PCM 语义一致。
"""
from __future__ import annotations

import math
import struct
import threading

#: 与固件 haptic_synth.c 同刻度：amp 255 的 int16 峰值与采样脉冲峰值。
AMP_MAX = 24000
PULSE_AMP = 20000
PULSE_FREQ = 190.0
RATE = 48000
CHANNELS = 4
#: 频率缺省值（设备发的落地值理论上不为 0，这里兜底）。
FREQ_DEFAULTS = (55.0, 190.0)
_TAU = 2.0 * math.pi


class Ds5HapticsAudio:
    """一条对着 DualSense 音频端点的 4ch int16 输出流 + 参数驱动的正弦合成。

    set_params 可从会话循环任意调用（内部加锁），合成跑在音频回调线程里，
    相位逐块推进、换参数不重置（拼接处不跳变）。
    """

    def __init__(self, reporter=None) -> None:
        self._reporter = reporter
        self._lock = threading.Lock()
        self._params = {
            "lf_amp": (0, 0),
            "hf_amp": (0, 0),
            "lf_freq": (FREQ_DEFAULTS[0], FREQ_DEFAULTS[0]),
            "hf_freq": (FREQ_DEFAULTS[1], FREQ_DEFAULTS[1]),
            "pulse": 0,
        }
        # 每侧两带的振荡器相位（弧度，按 2π 取模防精度漂移）。
        self._phase = [[0.0, 0.0], [0.0, 0.0]]
        self._pulse_phase = 0.0
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
        """吃 link.feedback_params 的解析结果：振幅随两带、频率落地值、采样脉冲。

        sample 是固件音色表渲染出的当前幅度（0-255，0 = 停顿段）——采样的
        播放节奏在固件里生成，这里只按幅度哑渲染。"""
        lf_freq = params.get("lf_freq") or (FREQ_DEFAULTS[0], FREQ_DEFAULTS[0])
        hf_freq = params.get("hf_freq") or (FREQ_DEFAULTS[1], FREQ_DEFAULTS[1])
        with self._lock:
            self._params = {
                "lf_amp": tuple(params.get("lf_amp") or (0, 0)),
                "hf_amp": tuple(params.get("hf_amp") or (0, 0)),
                "lf_freq": tuple(float(f) or FREQ_DEFAULTS[0] for f in lf_freq),
                "hf_freq": tuple(float(f) or FREQ_DEFAULTS[1] for f in hf_freq),
                "pulse": int(params.get("sample") or 0),
            }

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
        lf_amp = params["lf_amp"]
        hf_amp = params["hf_amp"]
        lf_freq = params["lf_freq"]
        hf_freq = params["hf_freq"]
        pulse_gain = params["pulse"] * PULSE_AMP // 255
        pulse_step = _TAU * PULSE_FREQ / RATE
        # 每侧两带的增益（int16）与角步进先算好，循环里只剩乘加。
        gains = [[lf_amp[s] * AMP_MAX // 255, hf_amp[s] * AMP_MAX // 255] for s in (0, 1)]
        steps = [[_TAU * lf_freq[s] / RATE, _TAU * hf_freq[s] / RATE] for s in (0, 1)]
        # 扬声器两路恒零；触觉两路按参数合成。
        block = bytearray(frames * CHANNELS * 2)
        pack_into = struct.pack_into
        off = 0
        for _i in range(frames):
            samples = [0, 0]
            for side in (0, 1):
                total = 0
                for band in (0, 1):
                    if gains[side][band] > 0:
                        total += int(gains[side][band] * math.sin(self._phase[side][band]))
                    self._phase[side][band] = (self._phase[side][band] + steps[side][band]) % _TAU
                if pulse_gain > 0:
                    total += int(pulse_gain * math.sin(self._pulse_phase))
                    self._pulse_phase = (self._pulse_phase + pulse_step) % _TAU
                if total > 32767:
                    total = 32767
                elif total < -32768:
                    total = -32768
                samples[side] = total
            pack_into("<4h", block, off, 0, 0, samples[0], samples[1])
            off += CHANNELS * 2
        outdata[:] = bytes(block)

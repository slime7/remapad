# -*- coding: utf-8 -*-
"""把本目录的 .capture 采集样本回放到 DualSense 手柄。

回放引擎镜像固件的转换链（ns2_output 解码 → pad_feedback_hd_render 落地 →
采样音色时间线），落点四选一：
- usb  直插 PC 的 DS5：WASAPI 4ch 音频流（频道 3/4 音圈、1/2 小喇叭），HD 全保真；
- bt   蓝牙 DS5：0x31 HID 双马达近似（重击马达跟低频、纹理马达跟高频，蜂鸣段
  无法渲染）；
- bt32 蓝牙 DS5：0x32 私有触觉流（SAxense 142 字节原始形态直写），发声段折进
  音圈（摸得到、听不到）；
- bt36 蓝牙 DS5：0x36 私有触觉+喇叭流（vds 形态，HD 触觉 + 手柄喇叭真声），
  需要 PyAV/libopus，不可用时自动回落 bt32。
- bt39 蓝牙 DS5：0x39 成对形态（一报 2 块触觉 + 2 帧喇叭，节拍 21.33ms）——
  链路抖动的水垫翻倍，用来对比单块形态的漏震。

回放中 Ctrl-C 随时干净退出（音频流与 HID 句柄都会收尾）。

用法（仓库根）：
  uv run python pc/tests/samples/pad_replay.py --list
  uv run python pc/tests/samples/pad_replay.py ns2-gameplay-rumble.capture
  uv run python pc/tests/samples/pad_replay.py ns2-search-page.capture --pad bt36 --speed 0.5
  uv run python pc/tests/samples/pad_replay.py ns2-search-page.capture --pad bt39
"""
import argparse
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
import ds5_haptics  # noqa: E402

SAMPLES_DIR = Path(__file__).resolve().parent

# ---------------------------------------------------------------- 采集解析

_LINE_RE = re.compile(
    r"\s*\+([\d.]+)s (\w+)\[0x([0-9a-f]+)\] seq=(\d+)\s+(\d+)B\s+(.*)$")


def load_capture(path: Path) -> list[tuple[float, str, bytes]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        m = _LINE_RE.match(line)
        if m is None:
            raise ValueError(f"看不懂的抓包行：{line}")
        payload = bytes.fromhex(m.group(6).strip())
        rows.append((float(m.group(1)), m.group(2), payload))
    if not rows:
        raise ValueError(f"{path.name} 里没有记录")
    return rows


# ---------------------------------------------------------------- 固件镜像

OCT_FRAC = [92682, 77935, 71461, 68442, 66965, 66229, 65878]
FREQ_MIN, FREQ_MAX, FREQ_DEFAULT = 20, 500, (80, 135)
CARRIER_MAX = 2
#: 采样音色表（与 pad/feedback.c 的 s_haptic_bank 同数据）：段边界 ms → (幅度, 音高)。
LOCATE_STEPS = [(220, 0xC0, 0), (400, 0, 0), (500, 0x80, 880),
                (600, 0, 0), (700, 0x80, 1175), (1200, 0, 0)]
LF_BEEP_STEPS = [(1000, 0xC0, 0), (1100, 0, 0)]
DEFAULT_STEPS = [(120, 0xC0, 0), (300, 0, 0)]
HAPTIC_HOLD_MS = 300


def key_freq_hz(code: int) -> int:
    """固件 key_freq_hz 的镜像：9 位 log2 频率码 → Hz，码 0 = 未声明。"""
    if code == 0:
        return 0
    code = min(code, 0x1FF)
    f = (10 << (code >> 7)) << 16
    frac = code & 0x7F
    for i in range(7):
        if frac & (0x40 >> i):
            f = ((f * OCT_FRAC[i]) + 0x8000) >> 16
    return (f + 0x8000) >> 16


def hd_freq(raw: int, high: bool) -> int:
    df = FREQ_DEFAULT[1] if high else FREQ_DEFAULT[0]
    if raw == 0:
        raw = df
    return max(FREQ_MIN, min(raw, FREQ_MAX))


def decode_keys(raw16: bytes) -> list[dict]:
    """ns2_rumble_keys 镜像：状态字 bit4-5 声明有效子帧数，逐子帧解 40 位位串。"""
    keys = []
    for g in range(3):
        v = int.from_bytes(raw16[1 + g * 5:6 + g * 5], "little")
        keys.append({
            "lf_freq": key_freq_hz(v & 0x1FF),
            "lf_amp": (v >> 10) & 0x3FF,
            "hf_freq": key_freq_hz((v >> 20) & 0x1FF),
            "hf_amp": ((v >> 32) & 0xFF) << 2,
        })
    declared = (raw16[0] >> 4) & 0x3
    return keys[:declared] if 0 < declared < 3 else keys


def declared_count(side_keys: list[dict]) -> int:
    """主机声明的子帧数（0 = 未声明，按满 3 处理）：与固件 `ns2_rumble_keys`
    同一语义，轮播长度就是它——声明之外的槽位不占时间。"""
    return len(side_keys) if 0 < len(side_keys) < 3 else 3


def hd_render(side_keys: list[dict]) -> list[dict]:
    """pad_feedback_hd_render 的子帧规则：有效子帧振幅 >>2 直迁、频率夹取
    回落，无效子帧静默。"""
    out = []
    for k in range(3):
        if k < len(side_keys):
            src = side_keys[k]
            active = src["lf_amp"] != 0 or src["hf_amp"] != 0
            out.append({
                "lf_freq": hd_freq(src["lf_freq"], False) if active else 0,
                "lf_gain": src["lf_amp"] >> 2 if active else 0,
                "hf_freq": hd_freq(src["hf_freq"], True) if active else 0,
                "hf_gain": src["hf_amp"] >> 2 if active else 0,
            })
        else:
            out.append({"lf_freq": 0, "lf_gain": 0, "hf_freq": 0, "hf_gain": 0})
    return out


def pulse_step(steps, age_ms):
    """pad_haptic_pulse_step 的镜像：返回 (幅度, 音高)。"""
    period = steps[-1][0]
    ms = age_ms % period
    for until, amp, hz in steps:
        if ms < until:
            return amp, (hz if amp else 0)
    return 0, 0


def scaled_key(key: dict, gain: float) -> dict:
    """触觉增益倍率（A/B 标定用）：只改增益、夹回 0-255，频率与段边界不动。

    主机的游戏内档位很小，线性直迁到音圈几乎摸不到；DS5 两行的 `hd` 规则取
    4 倍（`gain_num/gain_den`，落地在 `pad_feedback_hd_render`，板载合成与
    PC 哑渲染吃同一份数值），这里的默认值与它对齐；传 1.0 回原始刻度做对比。
    """
    out = dict(key)
    out["lf_gain"] = max(0, min(255, int(key["lf_gain"] * gain + 0.5)))
    out["hf_gain"] = max(0, min(255, int(key["hf_gain"] * gain + 0.5)))
    return out


class FeedbackSim:
    """主机反馈状态机：吃采集记录，按时间给出「这一拍该渲染什么」。

    hd_gain 是触觉增益倍率（默认 1.0 = 与固件同刻度），见 `scaled_key`。"""

    def __init__(self, records: list[tuple[float, str, bytes]], hd_gain: float = 1.0):
        self.records = records
        self.hd_gain = hd_gain
        self.t0 = records[0][0]
        self.rumble_raw = {0: None, 1: None}
        #: 每侧主机声明的子帧数（固件 key_count 的同一语义：声明之外的槽位
        #: 不占时间，轮播长度就是它）。
        self.key_count = {0: 3, 1: 3}
        self.sample = None
        self.sample_start = None
        self.sample_last = None

    def count(self, side: int) -> int:
        """该侧当前声明的子帧数（合成强震段铺满 3 槽，因此恒为 3）。"""
        return self.key_count[side]

    def at(self, now_ms: float):
        """推进到 now_ms（相对采集起点），返回 (keys_l, keys_r, speaker)。"""
        while self.records and (self.records[0][0] - self.t0) * 1000.0 <= now_ms:
            t, name, payload = self.records.pop(0)
            if name == "rumble" and len(payload) >= 33:
                self.rumble_raw = {0: payload[1:17], 1: payload[17:33]}
                self.key_count = {side: declared_count(decode_keys(raw))
                                  for side, raw in self.rumble_raw.items()}
            elif name == "composite" and len(payload) >= 45:
                frame = payload[33:]
                if frame[0] == 0x0A and len(frame) >= 9:
                    sample = frame[8] if frame[3] == 0x02 else frame[3]
                    if sample:
                        if self.sample != sample:
                            self.sample_start = t
                        self.sample = sample
                    else:
                        self.sample = None
                    self.sample_last = t

        keys = {side: hd_render(decode_keys(raw)) if raw else
                hd_render([]) for side, raw in self.rumble_raw.items()}
        speaker = (0, 0)
        if self.sample is not None and self.sample_last is not None:
            age = (self.sample_last - self.sample_start) * 1000.0
            steps = LOCATE_STEPS if self.sample == 0x02 else \
                LF_BEEP_STEPS if self.sample == 0x01 else DEFAULT_STEPS
            if (now_ms / 1000.0 - self.sample_last) * 1000.0 <= HAPTIC_HOLD_MS:
                amp, tone = pulse_step(steps, age)
                if amp == 0xC0:  # 强震段覆盖两侧音圈
                    for side in (0, 1):
                        keys[side] = [{"lf_freq": 135, "lf_gain": 255,
                                       "hf_freq": 0, "hf_gain": 0}] * 3
                        self.key_count[side] = 3
                elif amp == 0x80:  # 发声段铺扬声器
                    speaker = (tone, 255)
        if self.hd_gain != 1.0:
            for side in (0, 1):
                keys[side] = [scaled_key(k, self.hd_gain) for k in keys[side]]
        return keys, speaker


# ---------------------------------------------------------------- 落点

def find_pad(conn: str):
    """按连接方式挑 DS5 的 gamepad 接口（usage 01/04 或 01/05）。

    连接方式看 HID 的 bus_type（1 = USB、2 = 蓝牙），不看 PID：0x0DF2 既是
    DualSense 的蓝牙 PID，也是 DualSense Edge 的有线 PID（同一个号），按 PID
    猜会把直插的 Edge 判成蓝牙——产品侧 remapadctl.conn_for 用的也是 bus_type。
    bus_type 缺失（旧 hidapi）时才退回 PID 判据。
    """
    import hid
    for info in hid.enumerate(0x054C):
        if info["product_id"] not in (0x0CE6, 0x0DF2):
            continue
        if info.get("usage_page") != 0x01 or info.get("usage") not in (0x04, 0x05):
            continue
        bus = info.get("bus_type")
        is_bt = bus == 2 if bus in (1, 2) else info["product_id"] == 0x0DF2
        if (conn == "bt" and is_bt) or (conn == "usb" and not is_bt):
            return info
    return None


def opus_available():
    """0x36 的喇叭帧要 Opus 编码器（PyAV）；缺失时同一路私有流退到 0x32。"""
    try:
        ds5_haptics.Bt36OpusEncoder()
    except Exception:  # noqa: BLE001 - 只判定可用性，缺依赖/缺库都算不可用
        return False
    return True


def resolve_pad(choice, usb_info, bt_info):
    """解析 --pad 的实际落点，auto 跟产品固件的蓝牙通路保持一致。

    固件侧蓝牙默认就走私有触觉流（有 0x36 用 0x36，缺 Opus 退 0x32），auto
    跟着这条路走：直插 DS5 在就用 USB 音频端点，否则落蓝牙私有流；只有显式
    给 bt 才回 0x31 的双马达近似。
    """
    if choice != "auto":
        return choice
    if usb_info is not None:
        return "usb"
    if bt_info is None:
        return "bt"
    return "bt36" if opus_available() else "bt32"


def open_hid(info):
    import hid
    dev = hid.device()
    dev.open_path(info["path"])
    return dev


def crc32_le(crc, data):
    for b in data:
        crc ^= b
        for _ in range(8):
            mask = (crc & 1) * 0xEDB88320
            crc = (crc >> 1) ^ mask
    return crc


#: DS5 输出报告的公共段预置（与固件 pad/layouts/ds5.c 两行的 preset 同一份
#: 口径）：b3/b4（有线 b1/b2）是有效位——兼容震动 + 关音频触觉 + 更新喇叭音量
#: + 音频控制 + 前级增益；喇叭音量钉 100（PS5 缺省档）；输出路径位段
#: （无线的 b10 / 有线的 b8）置 0x30 = 手柄喇叭，前级 +6dB。
#: 不写输出路径时手柄内置喇叭处在未路由状态，发声段到哪里都不出声。
PRESET_0X31 = {2: 0x10, 3: 0xA3, 4: 0x90, 8: 100, 10: 0x30, 40: 0x02}
PRESET_0X02 = {1: 0xA3, 2: 0x90, 6: 100, 8: 0x30, 38: 0x02}


def build_setup_0x31(seq):
    """蓝牙喇叭路由报告（0x31）：只写预置，不驱动马达。回放前先发一份，手柄
    内置喇叭才有路由（不路由时 0x36 触觉可达而喇叭无声）。"""
    out = bytearray(78)
    out[0] = 0x31
    out[1] = (seq & 0xF) << 4
    for off, val in PRESET_0X31.items():
        out[off] = val
    c = crc32_le(crc32_le(0xFFFFFFFF, b"\xA2"), bytes(out[:74]))
    out[74:78] = struct.pack("<I", (~c) & 0xFFFFFFFF)
    return bytes(out)


def build_setup_0x02():
    """有线喇叭路由与音量报告（0x02，48 字节，无 CRC）：WASAPI 4ch 的发声段
    同样靠它出声。"""
    out = bytearray(48)
    out[0] = 0x02
    for off, val in PRESET_0X02.items():
        out[off] = val
    return bytes(out)


def build_rumble_0x31(seq, left, right):
    """DS5 蓝牙 0x31 震动报告（预置同固件布局行，尾部 CRC 与固件 ps_bt_frame
    同配方）。"""
    out = bytearray(78)
    out[0] = 0x31
    out[1] = (seq & 0xF) << 4
    for off, val in PRESET_0X31.items():
        out[off] = val
    out[5] = right
    out[6] = left
    c = crc32_le(crc32_le(0xFFFFFFFF, b"\xA2"), bytes(out[:74]))
    out[74:78] = struct.pack("<I", (~c) & 0xFFFFFFFF)
    return bytes(out)


def perceived_amp(amp):
    """固件 pad_rumble_perceived 的镜像（out = 40 + 215·√(amp/255)，非零档不低于
    53）：主机的小档位直迁到 ERM/马达字节整段落进死区，回放落点要与固件写回
    看到同一个值，手感才有可比性。"""
    if amp <= 0:
        return 0
    value = 40 + 215.0 * (amp / 255.0) ** 0.5
    return max(53, min(255, int(value + 0.5)))


def run_bt_hid(dev, sim, total_ms, speed):
    """蓝牙 0x31 落点：每 15ms 一拍，两颗马达按固件布局行的分带驱动（左大马达
    跟低频、右小马达跟高频），振幅过同一条感知曲线，发声段无法渲染。"""
    print("蓝牙 0x31 HID 震动回放（HD 纹理压成两带马达，发声段无法渲染）")
    seq = 0
    sent = [0, 0]
    tick = 0
    durations = []
    while tick * 15.0 <= total_ms:
        keys, _speaker = sim.at(tick * 15.0)
        left = perceived_amp(max((k["lf_gain"] for k in keys[0]), default=0))
        right = perceived_amp(max((k["hf_gain"] for k in keys[1]), default=0))
        t0 = time.monotonic()
        dev.write(build_rumble_0x31(seq, left, right))
        durations.append(time.monotonic() - t0)
        seq = (seq + 1) & 0xF
        sent[0 if left or right else 1] += 1
        time.sleep(0.015 / speed)
        tick += 1
    for _ in range(3):
        dev.write(build_rumble_0x31(seq, 0, 0))
        seq = (seq + 1) & 0xF
    print(f"回放结束：震动 {sent[0]} 拍、静默 {sent[1]} 拍")
    print_report(durations)


def prime_speaker_route(dev):
    """回放前先写一份喇叭路由与音量档（0x31）：手柄内置喇叭未路由时，0x36 的
    Opus 喇叭块送进去也一声不出。返回是否写成功（失败只提示，不拦回放）。"""
    try:
        dev.write(build_setup_0x31(0))
        return True
    except OSError as exc:
        print(f"喇叭路由报告写回失败（{exc}），喇叭可能不出声")
        return False


def run_bt_32(dev, sim, total_ms, speed):
    """蓝牙 0x32 HD 落点：SAxense 142 字节原始形态直写（描述符按 141 字节数据
    声明 0x32，不填充），发声段折进音圈。"""
    print("蓝牙 0x32 私有触觉回放（142 字节原始形态，发声段折进音圈）")
    prime_speaker_route(dev)
    state = ds5_haptics._VoiceState()
    seq = 0
    durations = []
    next_due = time.monotonic()
    tick_ms = 0.0
    while tick_ms <= total_ms:
        keys, speaker = sim.at(tick_ms)
        pcm = ds5_haptics.bt_render_pcm(
            {"count": sim.count(0), "keys": tuple(_key_tuples(keys[0]))},
            {"count": sim.count(1), "keys": tuple(_key_tuples(keys[1]))},
            (speaker,), state)
        t0 = time.monotonic()
        dev.write(ds5_haptics.bt_build_report(pcm, seq))
        durations.append(time.monotonic() - t0)
        seq = (seq + 1) & 0xFF
        next_due += ds5_haptics.BT_INTERVAL_S / speed
        tick_ms += ds5_haptics.BT_INTERVAL_S * 1000.0 * speed
        now = time.monotonic()
        time.sleep(max(0.0, min(next_due - now, 0.05)))
    print("回放结束")
    print_report(durations)


def run_bt_36(dev, sim, total_ms, speed, encoder):
    """蓝牙 0x36 HD+喇叭落点：vds 398 字节形态，节拍按一报里触觉 PCM 的时长
    （10.67ms），触觉不折喇叭、发声段由手柄喇叭真声播放。"""
    print("蓝牙 0x36 私有触觉+喇叭回放（HD 触觉 + 手柄喇叭真声）")
    prime_speaker_route(dev)
    state = ds5_haptics._VoiceState()
    state_beat = ds5_haptics._VoiceState()
    seq = 0
    packet_seq = 0
    durations = []
    interval = ds5_haptics.BT_INTERVAL_S
    next_due = time.monotonic()
    tick_ms = 0.0
    while tick_ms <= total_ms:
        keys, speaker = sim.at(tick_ms)
        coil = ds5_haptics.bt_render_pcm(
            {"count": sim.count(0), "keys": tuple(_key_tuples(keys[0]))},
            {"count": sim.count(1), "keys": tuple(_key_tuples(keys[1]))},
            (), state)
        block = encoder.encode(
            ds5_haptics.render_speaker_beat(speaker if speaker else (0, 0), state_beat))
        t0 = time.monotonic()
        dev.write(ds5_haptics.bt36_build_report(coil, block,
                                                report_seq=seq,
                                                packet_seq=packet_seq))
        durations.append(time.monotonic() - t0)
        seq = (seq + 1) & 0xF
        packet_seq = (packet_seq + 1) & 0xFF
        next_due += interval / speed
        tick_ms += interval * 1000.0 * speed
        now = time.monotonic()
        time.sleep(max(0.0, min(next_due - now, 0.05)))
    print("回放结束")
    print_report(durations)


def _key_tuples(rendered):
    return tuple(((k["lf_freq"], k["lf_gain"]), (k["hf_freq"], k["hf_gain"]))
                 for k in rendered)


def run_bt_39(dev, sim, total_ms, speed, encoder):
    """蓝牙 0x39 成对落点：一报 2 块触觉（128 字节 PCM）+ 2 帧喇叭（547 字节
    形态），节拍 21.33ms。多带的那一块是链路抖动的水垫——单块形态下一拍迟到
    10.67ms 就断音，成对形态的容差翻倍，报数减半也少一半链路开销。"""
    print("蓝牙 0x39 成对触觉+喇叭回放（一报 2 块，链路容差翻倍）")
    prime_speaker_route(dev)
    state = ds5_haptics._VoiceState()
    state_beat = ds5_haptics._VoiceState()
    seq = 0
    packet_seq = 0
    durations = []
    interval = ds5_haptics.BT39_INTERVAL_S
    next_due = time.monotonic()
    tick_ms = 0.0
    while tick_ms <= total_ms:
        keys, speaker = sim.at(tick_ms)
        coil = ds5_haptics.bt_render_pcm(
            {"count": sim.count(0), "keys": tuple(_key_tuples(keys[0]))},
            {"count": sim.count(1), "keys": tuple(_key_tuples(keys[1]))},
            (), state, frames=ds5_haptics.BT_FRAMES * 2)
        block = encoder.encode(
            ds5_haptics.render_speaker_pair(speaker if speaker else (0, 0), state_beat))
        t0 = time.monotonic()
        dev.write(ds5_haptics.bt39_build_report(coil, block,
                                                report_seq=seq,
                                                packet_seq=packet_seq))
        durations.append(time.monotonic() - t0)
        seq = (seq + 1) & 0xF
        packet_seq = (packet_seq + 1) & 0xFF
        next_due += interval / speed
        tick_ms += interval * 1000.0 * speed
        now = time.monotonic()
        time.sleep(max(0.0, min(next_due - now, 0.05)))
    print("回放结束")
    print_report(durations)


def run_usb(sim, total_ms, speed):
    """USB 落点：WASAPI 4ch 流，哑渲染吃 HD 子帧与扬声器音色（全保真）。

    开流前先经 HID 写一份 0x02 的路由与音量档（与固件写回同一份口径）：内置
    喇叭未路由、或手柄音量档被主机压低时，4ch 的发声段听着就像没声音。"""
    route_dev = None
    info = find_pad("usb")
    if info is not None:
        try:
            route_dev = open_hid(info)
            route_dev.write(build_setup_0x02())
        except OSError as exc:
            print(f"路由/音量报告写回失败（{exc}），喇叭可能不出声")
            if route_dev is not None:
                route_dev.close()
                route_dev = None
    audio = ds5_haptics.Ds5HapticsAudio()
    if not audio.start():
        print("DualSense 音频端点打不开（被占用或不是直插 USB）")
        if route_dev is not None:
            route_dev.close()
        return
    print("USB 音频触觉回放（4ch WASAPI，HD 全保真）")
    try:
        tick_ms = 0.0
        while tick_ms <= total_ms:
            keys, speaker = sim.at(tick_ms)
            audio.set_params({"hd": {
                "l": {"count": sim.count(0), "keys": _key_tuples(keys[0])},
                "r": {"count": sim.count(1), "keys": _key_tuples(keys[1])},
                "speaker": speaker}})
            time.sleep(0.005 / speed)
            tick_ms += 5.0 * speed
    finally:
        audio.stop()
        if route_dev is not None:
            route_dev.close()
    print("回放结束")


def cmd_list():
    print("手柄：")
    import hid
    for info in hid.enumerate(0x054C):
        if info["product_id"] in (0x0CE6, 0x0DF2):
            conn = "蓝牙" if info["product_id"] == 0x0DF2 else "USB "
            print(f"  DualSense 054c:{info['product_id']:04x} {conn} "
                  f"usage {info.get('usage_page'):02x}/{info.get('usage'):02x}")
    print("样本：")
    for path in sorted(SAMPLES_DIR.glob("*.capture")):
        rows = load_capture(path)
        chans = {}
        for _t, name, _p in rows:
            chans[name] = chans.get(name, 0) + 1
        tail = ", ".join(f"{k}×{v}" for k, v in sorted(chans.items()))
        print(f"  {path.name}: {len(rows)} 条（{tail}），"
              f"{(rows[-1][0] - rows[0][0]):.0f} 秒")


def print_report(durations):
    """一段回放的写回耗时统计：平均/最大单次写回越接近节拍，链路越撑得住；
    明显超节拍说明报文被排队、触觉/声音会延迟播放。"""
    if not durations:
        return
    avg = sum(durations) / len(durations) * 1000.0
    mx = max(durations) * 1000.0
    slow = sum(1 for d in durations if d * 1000.0 > 10.67)
    print(f"  （写回 {len(durations)} 份：平均 {avg:.1f}ms、最大 {mx:.1f}ms、"
          f"超 10.67ms {slow} 份）", flush=True)


def main():
    # 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（重定向到 StringIO
    # 时没有 reconfigure，跳过即可）。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("capture", nargs="?", help="要回放的 .capture 样本")
    parser.add_argument("--pad", choices=("auto", "usb", "bt", "bt32", "bt36", "bt39"),
                        default="auto",
                        help="回放落点：auto = 有线优先、否则蓝牙私有触觉流"
                             "（默认，有 Opus 走 0x36、缺了退 0x32）；"
                             "bt = 0x31 HID 双马达；bt32 = 0x32 HD（折进音圈）；"
                             "bt36 = 0x36 HD + 喇叭；bt39 = 0x39 成对形态"
                             "（一报 2 块，链路容差翻倍）")
    parser.add_argument("--speed", type=float, default=1.0, help="回放速度倍率")
    parser.add_argument("--hd-gain", type=float, default=4.0,
                        help="触觉增益倍率（默认 4.0 = 布局行 hd 规则的标定值；"
                             "主机游戏内档位小，线性直迁只占音圈满幅的百分之二上下，"
                             "传 1.0 可回原始刻度对比）")
    parser.add_argument("--list", action="store_true", help="列出手柄与样本后退出")
    args = parser.parse_args()

    if args.list:
        cmd_list()
        return
    try:
        if not args.capture:
            parser.error("给一个 .capture 样本，或用 --list")

        records = load_capture(SAMPLES_DIR / args.capture)
        total_ms = (records[-1][0] - records[0][0]) * 1000.0
        sim = FeedbackSim(records, hd_gain=args.hd_gain)
        print(f"回放 {args.capture}：{len(records)} 条记录，{total_ms / 1000.0:.0f} 秒，"
              f"{args.speed:.2f}x")

        conn = resolve_pad(args.pad, find_pad("usb"), find_pad("bt"))
        if args.pad == "auto":
            print(f"auto 落点：{conn}")
        if conn == "usb":
            run_usb(sim, total_ms, args.speed)
            return
        info = find_pad("bt")
        if info is None:
            print("没找到蓝牙连接的 DualSense")
            return
        dev = open_hid(info)
        try:
            if conn in ("bt32", "bt36", "bt39"):
                encoder = None
                if conn in ("bt36", "bt39"):
                    try:
                        encoder = ds5_haptics.Bt36OpusEncoder()
                    except Exception as exc:  # noqa: BLE001 - 缺依赖回落 0x32
                        print(f"0x36 喇叭流不可用（{exc}），回落 0x32")
                try:
                    if encoder is not None:
                        if conn == "bt39":
                            run_bt_39(dev, sim, total_ms, args.speed, encoder)
                        else:
                            run_bt_36(dev, sim, total_ms, args.speed, encoder)
                    else:
                        run_bt_32(dev, sim, total_ms, args.speed)
                except OSError as exc:
                    # 私有流写回被拒（句柄失效/链路不接受这类报文）：与产品路径
                    # 同语义，回落 HID 震动写回，整次回放不因此崩掉。
                    print(f"私有流写回被拒（{exc}），回落 0x31 HID 震动")
                    run_bt_hid(dev, sim, total_ms, args.speed)
            else:
                run_bt_hid(dev, sim, total_ms, args.speed)
        finally:
            dev.close()
    except KeyboardInterrupt:
        # Ctrl-C 随时打断：各落点的 finally 已停音频流/关 HID 句柄，这里只收尾。
        print("\n已中断（Ctrl-C）")
        raise SystemExit(130)


if __name__ == "__main__":
    main()

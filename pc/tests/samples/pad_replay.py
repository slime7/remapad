# -*- coding: utf-8 -*-
"""把本目录的 .capture 抓包样本回放到 DualSense 手柄，或发分段测试包。

回放引擎镜像固件的转换链（ns2_output 解码 → pad_feedback_hd_render 落地 →
采样音色时间线），落点四选一：
- usb  直插 PC 的 DS5：WASAPI 4ch 音频流（频道 3/4 音圈、1/2 小喇叭），HD 全保真；
- bt   蓝牙 DS5：0x31 HID 双马达近似（重击马达跟低频、纹理马达跟高频，蜂鸣段
  无法渲染）；
- bt32 蓝牙 DS5：0x32 私有触觉流（SAxense 142 字节原始形态直写），发声段折进
  音圈（摸得到、听不到）；
- bt36 蓝牙 DS5：0x36 私有触觉+喇叭流（vds 形态，HD 触觉 + 手柄喇叭真声），
  需要 PyAV/libopus，不可用时自动回落 bt32。

回放中 Ctrl-C 随时干净退出（音频流与 HID 句柄都会收尾）。

用法（pc/ 目录）：
  uv run python tests/samples/pad_replay.py --list
  uv run python tests/samples/pad_replay.py --test
  uv run python tests/samples/pad_replay.py ns2-gameplay-rumble.capture
  uv run python tests/samples/pad_replay.py ns2-search-page.capture --pad bt36 --speed 0.5
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

# ---------------------------------------------------------------- 抓包解析

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


class FeedbackSim:
    """主机反馈状态机：吃抓包记录，按时间给出「这一拍该渲染什么」。"""

    def __init__(self, records: list[tuple[float, str, bytes]]):
        self.records = records
        self.t0 = records[0][0]
        self.rumble_raw = {0: None, 1: None}
        self.sample = None
        self.sample_start = None
        self.sample_last = None

    def at(self, now_ms: float):
        """推进到 now_ms（相对抓包起点），返回 (keys_l, keys_r, speaker)。"""
        while self.records and (self.records[0][0] - self.t0) * 1000.0 <= now_ms:
            t, name, payload = self.records.pop(0)
            if name == "rumble" and len(payload) >= 33:
                self.rumble_raw = {0: payload[1:17], 1: payload[17:33]}
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
                elif amp == 0x80:  # 发声段铺扬声器
                    speaker = (tone, 255)
        return keys, speaker


# ---------------------------------------------------------------- 落点

def find_pad(conn: str):
    """按连接方式挑 DS5 的 gamepad 接口（usage 01/04 或 01/05）。"""
    import hid
    for info in hid.enumerate(0x054C):
        if info["product_id"] not in (0x0CE6, 0x0DF2):
            continue
        if info.get("usage_page") != 0x01 or info.get("usage") not in (0x04, 0x05):
            continue
        is_bt = info["product_id"] == 0x0DF2
        if (conn == "bt" and is_bt) or (conn == "usb" and not is_bt):
            return info
    return None


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


def build_rumble_0x31(seq, left, right):
    """DS5 蓝牙 0x31 震动报告（与固件 ps_bt_frame 同配方）。"""
    out = bytearray(78)
    out[0] = 0x31
    out[1] = (seq & 0xF) << 4
    out[2] = 0x10
    out[3] = 0x03
    out[4] = 0x10
    out[5] = right
    out[6] = left
    c = crc32_le(crc32_le(0xFFFFFFFF, b"\xA2"), bytes(out[:74]))
    out[74:78] = struct.pack("<I", (~c) & 0xFFFFFFFF)
    return bytes(out)


def run_bt_hid(dev, sim, total_ms, speed):
    """蓝牙 0x31 落点：每 15ms 一拍，两带强度驱动两颗马达，蜂鸣段无法渲染。"""
    print("蓝牙 0x31 HID 震动回放（HD 纹理压成两带马达，发声段无法渲染）")
    seq = 0
    sent = [0, 0]
    tick = 0
    while tick * 15.0 <= total_ms:
        keys, _speaker = sim.at(tick * 15.0)
        left = max((k["lf_gain"] for k in keys[0]), default=0)
        right = max((k["lf_gain"] for k in keys[1]), default=0)
        dev.write(build_rumble_0x31(seq, left, right))
        seq = (seq + 1) & 0xF
        sent[0 if left or right else 1] += 1
        time.sleep(0.015 / speed)
        tick += 1
    for _ in range(3):
        dev.write(build_rumble_0x31(seq, 0, 0))
        seq = (seq + 1) & 0xF
    print(f"回放结束：震动 {sent[0]} 拍、静默 {sent[1]} 拍")


def run_bt_32(dev, sim, total_ms, speed):
    """蓝牙 0x32 HD 落点：SAxense 142 字节原始形态直写（描述符按 141 字节数据
    声明 0x32，不填充），发声段折进音圈。"""
    print("蓝牙 0x32 私有触觉回放（142 字节原始形态，发声段折进音圈）")
    state = ds5_haptics._VoiceState()
    seq = 0
    next_due = time.monotonic()
    tick_ms = 0.0
    while tick_ms <= total_ms:
        keys, speaker = sim.at(tick_ms)
        pcm = ds5_haptics.bt_render_pcm(
            {"count": 3, "keys": tuple(_key_tuples(keys[0]))},
            {"count": 3, "keys": tuple(_key_tuples(keys[1]))},
            (speaker,), state)
        dev.write(ds5_haptics.bt_build_report(pcm, seq))
        seq = (seq + 1) & 0xFF
        next_due += ds5_haptics.BT_INTERVAL_S / speed
        tick_ms += ds5_haptics.BT_INTERVAL_S * 1000.0 * speed
        now = time.monotonic()
        time.sleep(max(0.0, min(next_due - now, 0.05)))
    print("回放结束")


def run_bt_36(dev, sim, total_ms, speed, encoder):
    """蓝牙 0x36 HD+喇叭落点：vds 398 字节形态，10ms 节拍，触觉不折喇叭、
    发声段由手柄喇叭真声播放。"""
    print("蓝牙 0x36 私有触觉+喇叭回放（HD 触觉 + 手柄喇叭真声）")
    state = ds5_haptics._VoiceState()
    state48 = ds5_haptics._VoiceState()
    seq = 0
    packet_seq = 0
    interval = ds5_haptics.BT36_INTERVAL_S
    next_due = time.monotonic()
    tick_ms = 0.0
    while tick_ms <= total_ms:
        keys, speaker = sim.at(tick_ms)
        coil = ds5_haptics.bt_render_pcm(
            {"count": 3, "keys": tuple(_key_tuples(keys[0]))},
            {"count": 3, "keys": tuple(_key_tuples(keys[1]))},
            (), state)
        block = encoder.encode(
            ds5_haptics.render_speaker_48k(speaker if speaker else (0, 0), state48))
        dev.write(ds5_haptics.bt36_build_report(coil, block,
                                                report_seq=seq,
                                                packet_seq=packet_seq))
        seq = (seq + 1) & 0xF
        packet_seq = (packet_seq + 1) & 0xFF
        next_due += interval / speed
        tick_ms += interval * 1000.0 * speed
        now = time.monotonic()
        time.sleep(max(0.0, min(next_due - now, 0.05)))
    print("回放结束")


def _key_tuples(rendered):
    return tuple(((k["lf_freq"], k["lf_gain"]), (k["hf_freq"], k["hf_gain"]))
                 for k in rendered)


def run_usb(sim, total_ms, speed):
    """USB 落点：WASAPI 4ch 流，哑渲染吃 HD 子帧与扬声器音色（全保真）。"""
    audio = ds5_haptics.Ds5HapticsAudio()
    if not audio.start():
        print("DualSense 音频端点打不开（被占用或不是直插 USB）")
        return
    print("USB 音频触觉回放（4ch WASAPI，HD 全保真）")
    try:
        tick_ms = 0.0
        while tick_ms <= total_ms:
            keys, speaker = sim.at(tick_ms)
            audio.set_params({"hd": {
                "l": {"count": 3, "keys": _key_tuples(keys[0])},
                "r": {"count": 3, "keys": _key_tuples(keys[1])},
                "speaker": speaker}})
            time.sleep(0.005 / speed)
            tick_ms += 5.0 * speed
    finally:
        audio.stop()
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


def cmd_test():
    """分段测试包（蓝牙手柄）：A = 0x31 老式双马达；B = 0x32 HD 135Hz；
    C = 0x32 两声上行短鸣（折进音圈）；D = 0x36 两声上行短鸣（手柄喇叭真声，
    需要 PyAV）。0x32 按 SAxense 142 字节、0x36 按 vds 398 字节直写。"""
    info = find_pad("bt")
    if info is None:
        print("没找到蓝牙连接的 DualSense")
        return
    dev = open_hid(info)

    try:
        encoder = ds5_haptics.Bt36OpusEncoder()
    except Exception as exc:  # noqa: BLE001 - 缺 PyAV/libopus 时跳过 D 段
        encoder = None
        print(f"（0x36 喇叭段跳过：{exc}）")

    def stream_0x32(build, seconds):
        state = ds5_haptics._VoiceState()
        seq = 0
        durations = []
        next_due = time.monotonic()
        end = next_due + seconds
        while True:
            t0 = time.monotonic()
            dev.write(ds5_haptics.bt_build_report(build(state), seq))
            durations.append(time.monotonic() - t0)
            seq = (seq + 1) & 0xFF
            next_due += ds5_haptics.BT_INTERVAL_S
            now = time.monotonic()
            if now >= end:
                break
            time.sleep(max(0.0, min(next_due - now, 0.02)))
        print_report(durations)

    def stream_0x36(speaker_tone, seconds):
        state = ds5_haptics._VoiceState()
        state48 = ds5_haptics._VoiceState()
        coil_state = ds5_haptics._VoiceState()
        seq = 0
        packet_seq = 0
        durations = []
        silent = {"count": 3, "keys": (((0, 0), (0, 0)),) * 3}
        next_due = time.monotonic()
        end = next_due + seconds
        while True:
            coil = ds5_haptics.bt_render_pcm(silent, silent, (), coil_state)
            block = encoder.encode(
                ds5_haptics.render_speaker_48k(speaker_tone, state48))
            t0 = time.monotonic()
            dev.write(ds5_haptics.bt36_build_report(coil, block,
                                                    report_seq=seq,
                                                    packet_seq=packet_seq))
            durations.append(time.monotonic() - t0)
            seq = (seq + 1) & 0xF
            packet_seq = (packet_seq + 1) & 0xFF
            next_due += ds5_haptics.BT36_INTERVAL_S
            now = time.monotonic()
            if now >= end:
                break
            time.sleep(max(0.0, min(next_due - now, 0.02)))
        print_report(durations)

    def print_report(durations):
        if not durations:
            return
        avg = sum(durations) / len(durations) * 1000.0
        mx = max(durations) * 1000.0
        slow = sum(1 for d in durations if d * 1000.0 > 10.67)
        print(f"  （写回 {len(durations)} 份：平均 {avg:.1f}ms、最大 {mx:.1f}ms、"
              f"超 10.67ms {slow} 份——平均越接近 10.67ms 链路越撑得住）", flush=True)

    strong = {"count": 3, "keys": (((135, 255), (0, 0)),) * 3}
    silent = {"count": 0, "keys": ()}
    print("3 秒后 A：0x31 老式双马达 1.5 秒", flush=True)
    time.sleep(3)
    for i in range(150):
        dev.write(build_rumble_0x31(i & 0xF, 80, 80))
        time.sleep(0.01)
    for i in range(3):
        dev.write(build_rumble_0x31(i & 0xF, 0, 0))
    print("A 结束", flush=True)
    time.sleep(4)
    print("B：0x32 HD 触觉 135Hz 2 秒（音圈震动）", flush=True)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(strong, strong, (), s), 2.0)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(silent, silent, (), s), 0.3)
    print("B 结束", flush=True)
    time.sleep(4)
    print("C：0x32 两声上行短鸣 880 → 1175Hz（只有触感，没有声音——发声段折进"
          "音圈，这是设计行为）", flush=True)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(silent, silent, ((880, 255),), s), 0.12)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(silent, silent, ((0, 0),), s), 0.12)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(silent, silent, ((1175, 255),), s), 0.12)
    stream_0x32(lambda s: ds5_haptics.bt_render_pcm(silent, silent, (), s), 0.3)
    print("C 结束", flush=True)
    if encoder is not None:
        time.sleep(4)
        print("D：0x36 两声上行短鸣 880 → 1175Hz（这一段才有喇叭声，应与提示同时"
              "出现）", flush=True)
        stream_0x36((880, 255), 0.14)
        stream_0x36((0, 0), 0.14)
        stream_0x36((1175, 255), 0.14)
        stream_0x36((0, 0), 0.3)
        print("D 结束，发送已全部停止——如果之后才听到声音，说明报文在链路上被"
              "排队延迟播放，把上面每段的写回统计发我", flush=True)
    else:
        print("D 跳过（无 Opus 编码器）")
    dev.close()


def main():
    # 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（重定向到 StringIO
    # 时没有 reconfigure，跳过即可）。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("capture", nargs="?", help="要回放的 .capture 样本")
    parser.add_argument("--pad", choices=("auto", "usb", "bt", "bt32", "bt36"),
                        default="auto",
                        help="回放落点：auto = USB 优先、否则蓝牙 0x31（默认）；"
                             "bt32 = 0x32 HD（折进音圈）；bt36 = 0x36 HD + 喇叭")
    parser.add_argument("--speed", type=float, default=1.0, help="回放速度倍率")
    parser.add_argument("--list", action="store_true", help="列出手柄与样本后退出")
    parser.add_argument("--test", action="store_true", help="发分段测试包后退出")
    args = parser.parse_args()

    if args.list:
        cmd_list()
        return
    try:
        if args.test:
            cmd_test()
            return
        if not args.capture:
            parser.error("给一个 .capture 样本，或用 --list / --test")

        records = load_capture(SAMPLES_DIR / args.capture)
        total_ms = (records[-1][0] - records[0][0]) * 1000.0
        sim = FeedbackSim(records)
        print(f"回放 {args.capture}：{len(records)} 条记录，{total_ms / 1000.0:.0f} 秒，"
              f"{args.speed:.2f}x")

        conn = args.pad
        if conn == "auto":
            conn = "usb" if find_pad("usb") else "bt"
        if conn == "usb":
            run_usb(sim, total_ms, args.speed)
            return
        info = find_pad("bt")
        if info is None:
            print("没找到蓝牙连接的 DualSense")
            return
        dev = open_hid(info)
        try:
            if conn in ("bt32", "bt36"):
                if conn == "bt36":
                    try:
                        encoder = ds5_haptics.Bt36OpusEncoder()
                    except Exception as exc:  # noqa: BLE001 - 缺依赖回落 0x32
                        print(f"0x36 喇叭流不可用（{exc}），回落 0x32")
                        encoder = None
                    if encoder is not None:
                        run_bt_36(dev, sim, total_ms, args.speed, encoder)
                        return
                run_bt_32(dev, sim, total_ms, args.speed)
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

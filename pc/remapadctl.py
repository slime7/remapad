#!/usr/bin/env python3
"""Remapad PC 侧单工具：桥接转发 + 串口命令行 + 实机截图 + 固件 OTA。

设备只有一根 Type-C：USB-Serial/JTAG 同时承载桥接帧、固件日志与 CLI 文本。
同一个进程持有这个口，因此转发手柄、敲命令、抓实机截图与推固件可以同时进行；
固件侧 input/input_link.c 按帧头分流，非帧字节交给 CLI 解析。

用法（在 pc/ 目录执行）：
    uv run python remapadctl.py --list                    # 枚举手柄接口
    uv run python remapadctl.py --dump --seconds 10       # 抓原始报告（不接串口）
    uv run python remapadctl.py -p COM3                   # 桥接 + 交互命令行
    uv run python remapadctl.py -p COM3 --no-pad          # 只当串口命令行用
    uv run python remapadctl.py -p COM3 status            # 一次性命令后退出
    uv run python remapadctl.py -p COM3 --all             # 拉取设备全部观测数据
    uv run python remapadctl.py -p COM3 --shot            # 实机截图存成 PNG
    uv run python remapadctl.py -p COM3 --log --seconds 20
    uv run python remapadctl.py -p COM3 --upgrade --wait

交互模式里不是 `:` 开头的行按固件 CLI 原样发送，手柄功能由此完整可控：
输入注入 key/stick、身份 ctrl、配对 pairing/wake/adv/drop、上报内容
motion/headset/fwver/fwpost/fwack/fwapply、链路 ltk/relay、反馈测试
rumble/lamp/haptic、屏幕 ui/backlight/screen、模式 mode（固件侧 help 有全表）。
数据命令都由固件现场读数应答，不经过 UI 层——UI 冻结（截图期间、页面门控
不取数）不影响 status/mem 等数据的实时性。
`:` 开头的是本工具命令：
    :help  :all  :shot [路径]  :log [秒]  :ota [镜像]  :quit

手柄转发默认只在交互模式里开：一次性命令、截图、只读日志与升级不碰手柄（否则主机会看到
手柄闪一下），要在这些模式里也转发就加 --pad，任何模式下都用 --no-pad 彻底关掉。

图形界面入口见同目录的 remapadgui.py：界面复用这里的会话循环与所有命令处理，
只是把输出换成队列、把键盘输入换成按钮与输入框；两边不要同时打开同一个串口。

依赖 hidapi（读手柄）与 link.py（串口 + 帧编解码）；细节见 pc/README.md。
"""

from __future__ import annotations

import argparse
import queue
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

from link import (
    CONN_BT,
    CONN_UNKNOWN,
    CONN_USB,
    FAMILY_NAMES,
    IMAGE_FORMAT_RGB565_LE,
    OTA_DATA_MAX,
    OTA_SLOT_WINDOW_END,
    OTA_WINDOW_FRAMES,
    TYPE_ATTACH,
    TYPE_DETACH,
    TYPE_FEEDBACK,
    TYPE_IMAGE_DATA,
    TYPE_IMAGE_END,
    TYPE_IMAGE_INFO,
    TYPE_OTA_ACK,
    TYPE_OTA_BEGIN,
    TYPE_OTA_DATA,
    TYPE_OTA_END,
    TYPE_OUT_REPORT,
    TYPE_PING,
    TYPE_REPORT,
    WIRE_MAX_PAYLOAD,
    FrameDecoder,
    SerialLink,
    device_id,
    encode,
    family_for_vendor,
    feedback_params,
    open_port,
    ota_begin_payload,
    ota_data_payload,
    parse_image_chunk,
    parse_image_end,
    parse_image_info,
    parse_ota_ack,
)

from ds5_haptics import Ds5HapticsAudio

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

#: 手柄类接口：Generic Desktop / Joystick 与 Game Pad。
GAMEPAD_USAGE_PAGE = 0x01
GAMEPAD_USAGES = (0x04, 0x05)

CONN_NAMES = {CONN_UNKNOWN: "-", CONN_USB: "usb", CONN_BT: "bt"}

#: 默认截图目录（相对本文件）与文件名前缀。
SHOT_DIR = Path(__file__).resolve().parent / "shots"
SHOT_PREFIX = "remapad-"


class HidUnavailable(RuntimeError):
    """hidapi 不可用：环境没装齐，工具的手柄功能无法工作。"""


class ImageError(RuntimeError):
    """应用镜像不合法：内容、芯片标识或项目名不符合本设备的升级条件。"""


class Reporter:
    """会话输出接收器：命令行写标准流，图形界面把同一批记录送进队列。"""

    def line(self, text: str) -> None:
        """一行普通输出（设备回复、固件日志、工具提示）。"""

    def error(self, text: str) -> None:
        """一行错误：命令行进 stderr，界面标红。"""

    def event(self, name: str, **fields) -> None:
        """结构化事件：手柄接入/断开、截图落盘、升级推进与结束、链路错误。"""


class ConsoleReporter(Reporter):
    """默认实现：保持工具原来的标准输出与标准错误行为。"""

    def line(self, text: str) -> None:
        print(text, flush=True)

    def error(self, text: str) -> None:
        print(text, file=sys.stderr, flush=True)

    def event(self, name: str, **fields) -> None:
        pass

# --- OTA：镜像校验常量（与固件 ota/ 的约定一致）---
ESP_IMAGE_MAGIC = 0xE9
ESP_CHIP_ID_OFFSET = 0x0C
ESP_CHIP_ID_ESP32S3 = 0x0009
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + 0x10
APP_DESC_PROJECT_OFFSET = APP_DESC_OFFSET + 0x30
APP_DESC_FIELD_LEN = 32
EXPECTED_PROJECT = "remapad_firmware"
PARTITION_MAX_BYTES = 4 * 1024 * 1024
DEFAULT_IMAGE = "../firmware/build/remapad_firmware.bin"

OTA_STATE_RECEIVING = 1
OTA_STATE_DONE = 2
OTA_STATE_FAILED = 3

#: 升级后等设备回来的超时与轮询间隔。
REOPEN_TIMEOUT_S = 60.0
REOPEN_INTERVAL_S = 1.0


#: 一键拉取设备数据的命令清单（每个都有无参回读；见 firmware/main/console/cli.c）。
ALL_QUERIES = (
    "status",
    "mem",
    "version",
    "link",
    "pad",
    "usb",
    "report",
    "ui",
    "adv",
    "headset",
    "fwver",
    "fwack",
    "fwpost",
    "fwapply",
    "ctrl",
    "backlight",
    "screen",
    "relay",
    "motion",
    "ltk",
    "rumble",
    "lamp",
    "haptic",
)


def load_hid():
    """导入 hidapi；缺失时抛 HidUnavailable，由调用方决定怎么报（命令行退出码 2）。"""
    try:
        import hid  # type: ignore
    except ImportError as exc:
        raise HidUnavailable("缺少 hidapi：在 pc/ 目录下执行 uv sync 后重试") from exc
    return hid


def list_candidates(hid) -> list[dict]:
    """枚举候选手柄接口：同一只手柄可能有多个 HID 接口，这里只留手柄用途的。"""
    found = []
    for info in hid.enumerate():
        if info.get("usage_page", 0) != GAMEPAD_USAGE_PAGE:
            continue
        if info.get("usage", 0) not in GAMEPAD_USAGES:
            continue
        found.append(info)
    return found


def conn_for(info: dict) -> int:
    bus = info.get("bus_type")
    if bus == 1:
        return CONN_USB
    if bus == 2:
        return CONN_BT
    return CONN_UNKNOWN


def describe(info: dict) -> str:
    return (f"{info.get('product_string') or '?'} "
            f"{info['vendor_id']:04x}:{info['product_id']:04x} "
            f"{FAMILY_NAMES.get(family_for_vendor(info['vendor_id']), '?')} "
            f"{CONN_NAMES.get(conn_for(info), '-')} if={info.get('interface_number')}")


def pick_device(args, hid) -> dict | None:
    """按 --vid / --pid 过滤候选，取第一只；图形界面用 args.pad_path 钉住具体接口。"""
    wanted_path = getattr(args, "pad_path", None)
    for info in list_candidates(hid):
        if args.vid is not None and info["vendor_id"] != args.vid:
            continue
        if args.pid is not None and info["product_id"] != args.pid:
            continue
        if wanted_path is not None and info["path"] != wanted_path:
            continue
        return info
    return None


def identity_payload(info: dict, report_id: int, report_len: int) -> bytes:
    return device_id(
        family_for_vendor(info["vendor_id"]),
        conn_for(info),
        info["vendor_id"],
        info["product_id"],
        report_id,
        report_len,
    )


def format_feedback(payload: bytes) -> str:
    params = feedback_params(payload)
    if params is None:
        return "反馈帧（载荷过短）"
    line = (f"反馈 震动 L={'on' if params['rumble_on'][0] else 'off'} "
            f"R={'on' if params['rumble_on'][1] else 'off'} "
            f"强度 {params['lf_amp'][0]}/{params['lf_amp'][1]} "
            f"玩家灯 0x{params['player_led']:02x} 触觉 0x{params['sample']:02x}")
    # 高频带（纹理）：解析里已按长度挡掉过短的老固件帧。
    line += f" 高频 {params['hf_amp'][0]}/{params['hf_amp'][1]}"
    if params["lf_freq"] is not None:
        # 两带驱动频率落地值（Hz）：音频触觉合成按它选频。
        line += (f" 频率 {params['lf_freq'][0]}/{params['lf_freq'][1]}"
                 f"+{params['hf_freq'][0]}/{params['hf_freq'][1]}")
    return line


class FeedbackThrottle:
    """反馈帧打印限频：震动效果包络里强度逐帧在变，逐条打印会把日志区刷爆
    （GUI 的 Tk 文本控件尤其扛不住每秒上百条）。窗口内只放行第一条，其余
    合并计数；窗口过后的下一条带上「已合并 N 条」。数据面（写回手柄）不受
    影响，这里只管打印。"""

    def __init__(self, window_s: float = 1.0) -> None:
        self.window_s = window_s
        self.next_ok = 0.0
        self.suppressed = 0

    def feed(self, payload: bytes, now: float) -> str | None:
        line = format_feedback(payload)
        if now < self.next_ok:
            self.suppressed += 1
            return None
        merged, self.suppressed = self.suppressed, 0
        self.next_ok = now + self.window_s
        return f"（已合并 {merged} 条）{line}" if merged else line


def run_list(hid) -> int:
    candidates = list_candidates(hid)
    if not candidates:
        print("没有找到手柄接口")
        return 1
    for info in candidates:
        print(describe(info))
    return 0


def run_dump(args, hid) -> int:
    """只打印原始报告：用来核对固件家族表里的字段偏移与位序。"""
    info = pick_device(args, hid)
    if info is None:
        if not list_candidates(hid):
            print("没有找到手柄接口（--list 可以看到全部候选）", file=sys.stderr)
        else:
            print("--vid/--pid 没有匹配到任何手柄接口（--list 可以看到全部候选）",
                  file=sys.stderr)
        return 1
    print(f"dump: {describe(info)}")
    dev = hid.device()
    dev.open_path(info["path"])
    dev.set_nonblocking(True)
    started = time.monotonic()
    try:
        while args.seconds <= 0 or time.monotonic() - started < args.seconds:
            data = dev.read(64)
            if not data:
                time.sleep(0.001)
                continue
            raw = bytes(data)
            stamp = time.monotonic() - started
            print(f"[{stamp:7.3f}s] len={len(raw):2d} {raw.hex(' ')}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()
    return 0


def default_shot_path() -> Path:
    return SHOT_DIR / f"{SHOT_PREFIX}{time.strftime('%Y%m%d-%H%M%S')}.png"


def write_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    """把 RGB565 小端像素写成 PNG：标准库 zlib + struct，不引入新依赖。"""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # 过滤器：none
        base = y * width * 2
        for x in range(width):
            value = pixels[base + 2 * x] | (pixels[base + 2 * x + 1] << 8)
            red = ((value >> 11) & 0x1F) * 255 // 31
            green = ((value >> 5) & 0x3F) * 255 // 63
            blue = (value & 0x1F) * 255 // 31
            raw += bytes((red, green, blue))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    body = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)


class ShotCollector:
    """按偏移拼一张实机截图；缺块或超时都不写文件。"""

    def __init__(self) -> None:
        self.width = 0
        self.height = 0
        self.format = 0
        self.total = 0
        self.received = 0
        self.chunks = 0
        self.buffer: bytearray | None = None
        self.deadline = 0.0
        self.ready = False
        self.error = ""

    def begin(self, now: float, timeout_s: float) -> None:
        self.width = 0
        self.height = 0
        self.format = 0
        self.total = 0
        self.received = 0
        self.chunks = 0
        self.buffer = None
        self.ready = False
        self.error = ""
        self.deadline = now + timeout_s

    def on_info(self, payload: bytes) -> None:
        width, height, pixel_format = parse_image_info(payload)
        self.width = width
        self.height = height
        self.format = pixel_format
        self.total = width * height * 2
        self.buffer = bytearray(self.total)
        self.received = 0
        self.chunks = 0
        if pixel_format != IMAGE_FORMAT_RGB565_LE:
            self.error = f"不认识的像素格式 0x{pixel_format:02x}"

    def on_data(self, payload: bytes) -> None:
        if self.buffer is None or self.error:
            return
        offset, data = parse_image_chunk(payload)
        if offset + len(data) > len(self.buffer):
            self.error = f"分块越界：偏移 {offset} + {len(data)} 字节"
            return
        self.buffer[offset:offset + len(data)] = data
        self.received += len(data)
        self.chunks += 1

    def on_end(self, payload: bytes) -> str:
        """收尾帧：按声明字节数与实收字节数判定完整性，返回结论（空串 = 完整）。"""
        if self.buffer is None:
            return "收到截图收尾帧，但没见过声明帧"
        if self.error:
            return f"截图失败：{self.error}"
        declared = parse_image_end(payload)
        if declared != self.total or self.received != self.total:
            return (f"截图不完整：收到 {self.received}/{self.total} 字节"
                    f"（设备声明 {declared}）")
        self.ready = True
        return ""


def describe_ack(ack: dict) -> str:
    version = f"，设备在跑 {ack['version']}" if ack.get("version") else ""
    return f"state={ack['state']} code={ack['code']}{version}"


class OtaJob:
    """升级窗口状态机：由会话主循环 tick 驱动，桥接转发同时照跑。"""

    BEGIN_ACK_TIMEOUT_S = 20.0
    ACK_TIMEOUT_S = 5.0
    END_ACK_TIMEOUT_S = 30.0
    MAX_WINDOW_RETRIES = 5

    def __init__(self, image: bytes, version: str, send, reporter: Reporter) -> None:
        self.image = image
        self.version = version
        self._send = send
        self.reporter = reporter
        self.confirmed = 0
        self.next_seq = 0
        self.retries = 0
        self.phase = "begin"
        self.deadline = 0.0
        self.printed_pct = -1
        self.finished = False
        self.exit_code = 1

    def start(self, now: float) -> None:
        self.reporter.line(f"写入 {len(self.image)} 字节（镜像版本 {self.version}）")
        self._send(encode(TYPE_OTA_BEGIN, 0, 0, ota_begin_payload(len(self.image)),
                          max_payload=WIRE_MAX_PAYLOAD))
        self.phase = "begin"
        self.deadline = now + self.BEGIN_ACK_TIMEOUT_S

    def _send_window(self, now: float) -> None:
        offset = self.confirmed
        seq = self.next_seq
        frames: list[tuple[int, bytes]] = []
        while len(frames) < OTA_WINDOW_FRAMES and offset < len(self.image):
            piece = self.image[offset:offset + OTA_DATA_MAX]
            frames.append((seq & 0xFFFF, piece))
            offset += len(piece)
            seq += 1
        window = bytearray()
        for index, (frame_seq, piece) in enumerate(frames):
            slot = OTA_SLOT_WINDOW_END if index == len(frames) - 1 else 0
            window += encode(TYPE_OTA_DATA, slot, frame_seq & 0xFF,
                             ota_data_payload(frame_seq, piece),
                             max_payload=WIRE_MAX_PAYLOAD)
        self._send(bytes(window))
        self.deadline = now + self.ACK_TIMEOUT_S

    def tick(self, now: float) -> None:
        if self.finished or now < self.deadline:
            return
        if self.phase == "begin":
            self._fail(f"设备没有在 {self.BEGIN_ACK_TIMEOUT_S:.0f} 秒内回应 BEGIN；"
                       "确认 COM 口没被别的程序占用、设备不是 host 模式")
            return
        if self.phase == "data":
            self.retries += 1
            if self.retries > self.MAX_WINDOW_RETRIES:
                self._fail(f"连续 {self.retries} 个窗口没有应答，升级中止；"
                           "设备侧 5 秒无数据会自行作废会话，仍从旧镜像启动")
                return
            self.reporter.line(f"窗口应答超时，从 {self.confirmed} 字节处重发（第 {self.retries} 次）")
            self._send_window(now)
            return
        if self.phase == "end":
            self._fail(f"设备没有在 {self.END_ACK_TIMEOUT_S:.0f} 秒内确认收尾")

    def on_ack(self, ack: dict) -> None:
        if self.finished:
            return
        if self.phase == "begin":
            if ack["state_id"] != OTA_STATE_RECEIVING or ack["code_id"] != 0:
                self._fail(f"设备拒绝升级（{describe_ack(ack)}）；设备忙或镜像被拒时稍后重试")
                return
            if ack["version"]:
                self.reporter.line(f"设备当前版本 {ack['version']} → 写入 {self.version}")
            self.phase = "data"
            self._send_window(time.monotonic())
            return
        if self.phase == "data":
            if ack["state_id"] == OTA_STATE_FAILED:
                self._fail(f"设备中止升级（{describe_ack(ack)}），已收到 {ack['received']} 字节")
                return
            self.retries = 0
            self.confirmed = ack["received"]
            self.next_seq = ack["next_seq"]
            pct = self.confirmed * 100 // max(len(self.image), 1)
            if pct != self.printed_pct:
                self.printed_pct = pct
                self.reporter.line(f"  写入 {pct:3d}%（{self.confirmed}/{len(self.image)} 字节）")
                self.reporter.event("ota_progress", confirmed=self.confirmed,
                                    total=len(self.image))
            now = time.monotonic()
            if self.confirmed >= len(self.image):
                self.phase = "end"
                self._send(encode(TYPE_OTA_END, 0, 0))
                self.deadline = now + self.END_ACK_TIMEOUT_S
                self.reporter.line("数据传输完成，等待设备校验镜像")
                return
            self._send_window(now)
            return
        if self.phase == "end":
            if ack["state_id"] == OTA_STATE_DONE and ack["code_id"] == 0:
                self.finished = True
                self.exit_code = 0
                self.reporter.line(
                    "升级完成：设备切到新分区并重启，首次启动会先处于「待验证」状态")
                self.reporter.event("ota_finished", ok=True, message="")
            else:
                self._fail(f"升级失败（{describe_ack(ack)}）；设备仍从旧镜像启动")

    def _fail(self, message: str) -> None:
        self.reporter.error(message)
        self.reporter.event("ota_finished", ok=False, message=message)
        self.finished = True
        self.exit_code = 1


def load_image(path: Path) -> tuple[bytes, str]:
    """读入并校验应用镜像，返回（字节, 版本号）；不合法抛 ImageError。"""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ImageError(f"读不到镜像 {path}：{exc}") from exc
    if len(data) < APP_DESC_PROJECT_OFFSET + APP_DESC_FIELD_LEN:
        raise ImageError(f"{path} 只有 {len(data)} 字节，不是应用镜像")
    if data[0] != ESP_IMAGE_MAGIC:
        raise ImageError(f"{path} 首字节是 0x{data[0]:02x}，不是 ESP-IDF 应用镜像")
    chip = int.from_bytes(data[ESP_CHIP_ID_OFFSET:ESP_CHIP_ID_OFFSET + 2], "little")
    if chip != ESP_CHIP_ID_ESP32S3:
        raise ImageError(f"{path} 的芯片标识是 0x{chip:04x}，不是 ESP32-S3")
    magic = int.from_bytes(data[APP_DESC_OFFSET:APP_DESC_OFFSET + 4], "little")
    if magic != APP_DESC_MAGIC:
        raise ImageError(f"{path} 缺少应用描述符（magic 0x{magic:08x}）")
    version = desc_field(data, APP_DESC_VERSION_OFFSET)
    project = desc_field(data, APP_DESC_PROJECT_OFFSET)
    if project != EXPECTED_PROJECT:
        raise ImageError(f"{path} 是 {project or '未知'} 的镜像，本设备只接受 {EXPECTED_PROJECT}")
    if len(data) > PARTITION_MAX_BYTES:
        raise ImageError(f"{path} 有 {len(data)} 字节，超过应用分区容量 {PARTITION_MAX_BYTES}")
    return data, version


def desc_field(data: bytes, offset: int) -> str:
    raw = data[offset:offset + APP_DESC_FIELD_LEN].split(b"\0")[0]
    return raw.decode("utf-8", errors="replace")


def wait_for_version(port: str, baud: int, reporter: Reporter | None = None) -> int:
    """等设备重启回来，问一次 version 命令并打印。"""
    out = reporter or ConsoleReporter()
    deadline = time.monotonic() + REOPEN_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(REOPEN_INTERVAL_S)
        try:
            ser = SerialLink(port, baud, read_timeout_ms=100)
        except OSError:
            continue
        try:
            ser.purge_input()
            ser.write(b"version\r")
            ser.flush()
            quit_at = time.monotonic() + 5.0
            while time.monotonic() < quit_at:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if line.startswith("fw="):
                    out.line(f"设备已回到 COM 口：{line}")
                    return 0
        finally:
            ser.close()
    out.error(f"{REOPEN_TIMEOUT_S:.0f} 秒内没有等到设备回到 {port}")
    return 1


class Session:
    """一个进程里的桥接 + 命令行会话：串口只有这一个持有者。"""

    def __init__(self, args, hid, ser: SerialLink, reporter: Reporter | None = None) -> None:
        self.args = args
        self.hid = hid
        self.link = ser
        # 输出接收器：命令行写标准流，图形界面送进队列（同一批文案两边共用）。
        self.reporter = reporter or ConsoleReporter()
        self.decoder = FrameDecoder()
        self.pad = None
        self.pad_info: dict | None = None
        self.ident = b""
        self.seq = 0
        self.reports = 0
        self.frames = 0
        self.outputs = 0
        self.next_send = 0.0
        self.next_rescan = 0.0
        self.interval = 1.0 / args.max_rate if args.max_rate > 0 else 0.0
        self.last_stat = time.monotonic()
        self.line_buf = ""
        self.print_logs = bool(args.logs or args.verbose)
        self.interactive = False
        self.log_mode = False
        self.log_raw = False
        self.log_started = 0.0
        self.log_deadline = 0.0
        self.one_shot = False
        self.expect_reply = False
        self.reply_seen = False
        self.reply_deadline = 0.0
        self.reply_hard_deadline = 0.0
        # 是否把手柄转发给设备：交互模式默认转发，一次性命令 / 截图 / 日志 /
        # 升级默认不碰手柄（否则主机会看到手柄闪一下），--pad 可显式打开。
        self.forward = False
        self.commands: queue.Queue[str] = queue.Queue()
        self.stdin_thread: threading.Thread | None = None
        self.shot = ShotCollector()
        self.shot_path: Path | None = None
        self.shot_ready = False
        self.ota: OtaJob | None = None
        self.stop = False
        self.stop_code = 0
        # 反馈帧打印限频（不影响写回手柄，见 FeedbackThrottle）。
        self.feedback_gate = FeedbackThrottle()
        # DS5 桥接时的 PC 侧音频触觉（attach 时按需启动）。
        self.haptics: Ds5HapticsAudio | None = None
        self._haptics_starting = False
        # 音频流开好后待发的「haptic audio on」：串口只允许主循环一个写者，
        # 后台线程只置这个标志。
        self._haptics_notify = False

    # --- 手柄转发 --------------------------------------------------

    def attach_pad(self, info: dict) -> None:
        device = self.hid.device()
        device.open_path(info["path"])
        device.set_nonblocking(True)
        self.pad = device
        self.pad_info = info
        self.ident = identity_payload(info, 0, 0)
        self.link.write(encode(TYPE_ATTACH, 0, self.seq, self.ident))
        description = describe(info)
        self.reporter.line(f"设备接入：{description}")
        self.reporter.event("pad_attached", describe=description)
        self.maybe_start_haptics()

    def maybe_start_haptics(self) -> None:
        """DS5 有线接入时启用 PC 侧音频触觉：对它的 4ch 音频端点合成触觉波形
        （通道 3/4），并告知固件把桥接写回的震动字段清零（haptic audio on）。
        --no-audio-haptics / --no-rumble 或端点开不起来时静默回落 HID 震动。
        WASAPI 开流要秒级、且不能占着桥接热路径，启动放后台线程。"""
        if self.haptics is not None or self._haptics_starting:
            return
        if getattr(self.args, "no_audio_haptics", False) or self.args.no_rumble:
            return
        info = self.pad_info or {}
        if (info.get("vendor_id") != 0x054C
                or info.get("product_id") not in (0x0CE6, 0x0DF2)
                or conn_for(info) != CONN_USB):
            return
        self._haptics_starting = True
        threading.Thread(target=self._start_haptics_worker, daemon=True).start()

    def _start_haptics_worker(self) -> None:
        audio = Ds5HapticsAudio(self.reporter)
        if not audio.start():
            self._haptics_starting = False
            return
        if self.pad is None:
            # 启动期间手柄已断开（会话收尾）：不留孤儿流。
            audio.stop()
            self._haptics_starting = False
            return
        self.haptics = audio
        self._haptics_starting = False
        # 「haptic audio on」由主循环发：串口句柄不跨线程写（worker 与主循环
        # 并发写会把命令字节冲烂，2026-09-18 实机抓到 err unknown command）。
        self._haptics_notify = True

    def pump_haptics_notify(self) -> None:
        if not self._haptics_notify or self.haptics is None:
            return
        self._haptics_notify = False
        try:
            self.send_cli("haptic audio on")
            self.reporter.line("DS5 音频触觉已启用（通道 3/4 合成，HID 震动让位）")
            self.reporter.event("haptics_audio", state="on")
        except OSError:
            self.stop_haptics()

    def stop_haptics(self) -> None:
        if self.haptics is None:
            return
        self.haptics.stop()
        self.haptics = None
        try:
            self.send_cli("haptic audio off")
            self.reporter.event("haptics_audio", state="off")
        except OSError:
            pass

    def detach_pad(self) -> None:
        self.stop_haptics()
        if self.pad is None:
            return
        try:
            self.link.write(encode(TYPE_DETACH, 0, self.seq, self.ident))
        except OSError:
            pass
        try:
            self.pad.close()
        except OSError:
            pass
        self.pad = None
        self.pad_info = None
        self.reporter.event("pad_detached")

    def pump_pad(self, now: float) -> None:
        if self.hid is None or not self.forward:
            return
        if self.pad is None:
            if now < self.next_rescan:
                return
            self.next_rescan = now + 0.5
            info = pick_device(self.args, self.hid)
            if info is None:
                return
            self.attach_pad(info)
        elif (getattr(self.args, "pad_path", None) is not None
              and self.pad_info["path"] != self.args.pad_path):
            # 图形界面里换了手柄：在本任务内断开，下一轮重新接入选中的那只。
            self.detach_pad()
            return
        data = self.pad.read(64)
        if data and now >= self.next_send:
            raw = bytes(data)
            payload = identity_payload(self.pad_info, raw[0] if raw else 0, len(raw)) + raw
            self.link.write(encode(TYPE_REPORT, 0, self.seq, payload))
            self.seq = (self.seq + 1) & 0xFF
            self.reports += 1
            self.next_send = now + self.interval

    def write_output_report(self, payload: bytes) -> bool:
        """把设备编码好的输出报告写回手柄：布局知识只在固件里有一份。"""
        if self.args.no_rumble or self.pad is None or not payload:
            return False
        try:
            self.pad.write(payload)
        except OSError as exc:
            self.reporter.error(f"反馈写回失败：{exc}")
            return False
        return True

    # --- 链路读取 --------------------------------------------------

    def pump_link(self, now: float) -> None:
        chunk = self.link.read()
        if not chunk:
            return
        frames, text = self.decoder.feed(chunk)
        for frame_type, _slot, _seq, payload in frames:
            self.frames += 1
            if frame_type == TYPE_FEEDBACK:
                params = feedback_params(payload)
                if params is not None and self.haptics is not None:
                    self.haptics.set_params(params)
                line = self.feedback_gate.feed(payload, now)
                if line is not None:
                    self.reporter.line(line)
            elif frame_type == TYPE_OUT_REPORT:
                if self.write_output_report(payload):
                    self.outputs += 1
            elif frame_type == TYPE_PING:
                self.reporter.line(f"设备在线（协议 v{payload[0] if payload else 0}）")
            elif frame_type == TYPE_IMAGE_INFO:
                self.shot.on_info(payload)
            elif frame_type == TYPE_IMAGE_DATA:
                self.shot.on_data(payload)
            elif frame_type == TYPE_IMAGE_END:
                self.finish_shot(payload)
            elif frame_type == TYPE_OTA_ACK and self.ota is not None:
                self.ota.on_ack(parse_ota_ack(payload))
        if text:
            self.handle_text(text)

    def handle_text(self, text: bytes) -> None:
        self.line_buf += text.decode("utf-8", errors="replace")
        while "\n" in self.line_buf:
            line, self.line_buf = self.line_buf.split("\n", 1)
            self.handle_line(line.rstrip("\r"))

    def handle_line(self, line: str) -> None:
        if not line:
            return
        if self.log_mode and not self.log_raw and self.log_started:
            self.reporter.line(f"[{time.monotonic() - self.log_started:7.2f}s] {line}")
        else:
            self.reporter.line(line)
        if self.expect_reply:
            # 任何一行都算「设备还在说话」：status / link / pad 这类回复不以 ok 开头，
            # 固件日志也会夹在中间；每条新行把静默窗往后推，硬截止兜住总时长。
            quiet = 0.15 if line.startswith(("ok", "err", "pong")) else 0.25
            self.reply_seen = True
            self.reply_deadline = min(self.reply_hard_deadline or 1e9, time.monotonic() + quiet)

    # --- 命令行 ----------------------------------------------------

    def send_cli(self, command: str) -> None:
        self.link.write((command + "\r").encode("utf-8"))
        self.link.flush()

    def start_stdin(self) -> None:
        def reader() -> None:
            for line in sys.stdin:
                if self.stop:
                    break
                self.commands.put(line.rstrip("\r\n"))

        self.stdin_thread = threading.Thread(target=reader, daemon=True)
        self.stdin_thread.start()

    def pump_commands(self) -> None:
        while True:
            try:
                line = self.commands.get_nowait()
            except queue.Empty:
                return
            line = line.strip()
            if not line:
                continue
            if line.startswith(":"):
                self.run_local(line[1:])
            else:
                self.send_cli(line)

    def run_local(self, text: str) -> None:
        name, _, arguments = text.partition(" ")
        arguments = arguments.strip()
        parts = arguments.split()
        if name in ("help", "h", "?"):
            print_local_help(self.reporter)
        elif name == "all":
            self.run_all()
        elif name == "shot":
            # 截图路径整段当参数：路径里有空格也不用引号（图形界面的截图按钮同样走这里）。
            self.request_shot(arguments or None)
        elif name == "log":
            if parts and parts[0] == "off":
                self.log_mode = False
                self.reporter.line("日志透传关闭")
                return
            seconds = float(parts[0]) if parts else 15.0
            self.log_mode = True
            self.log_started = time.monotonic()
            self.log_deadline = self.log_started + seconds if seconds > 0 else 0.0
            self.reporter.line(f"透传设备日志 {seconds:.0f} 秒（0 表示持续到 :log off）")
        elif name == "ota":
            self.request_upgrade(arguments or None)
        elif name in ("quit", "q", "exit"):
            self.stop = True
        else:
            self.reporter.error(f"未知的工具命令：{name}（:help 看清单）")

    def request_shot(self, path: str | None) -> None:
        self.shot_path = Path(path).expanduser() if path else default_shot_path()
        self.shot_ready = False
        self.shot.begin(time.monotonic(), self.args.shot_timeout)
        self.send_cli("shot")

    def finish_shot(self, payload: bytes) -> None:
        if self.shot.buffer is None:
            return
        problem = self.shot.on_end(payload)
        if problem:
            self.reporter.error(problem)
            return
        # 交互模式里设备命令 shot 是用户直接敲的，走到这里才决定落盘路径。
        path = self.shot_path or default_shot_path()
        write_png(path, self.shot.width, self.shot.height, bytes(self.shot.buffer))
        self.shot_path = path
        self.reporter.line(f"截图已保存：{path}（{self.shot.width}x{self.shot.height}，"
                           f"{self.shot.chunks} 块）")
        self.reporter.event("shot_saved", path=str(path), width=self.shot.width,
                            height=self.shot.height, chunks=self.shot.chunks)
        self.shot_ready = True

    def request_upgrade(self, image_path: str | None) -> None:
        path = Path(image_path).expanduser() if image_path else Path(self.args.image)
        image, version = load_image(path)
        self.reporter.line(f"镜像 {path}：{len(image)} 字节，版本 {version}")
        self.reporter.event("ota_started", path=str(path), size=len(image), version=version)
        self.ota = OtaJob(image, version, self.link.write, self.reporter)
        self.ota.start(time.monotonic())

    # --- 主循环 ----------------------------------------------------

    def pump(self, now: float) -> None:
        self.pump_pad(now)
        self.pump_link(now)
        self.pump_commands()
        self.pump_haptics_notify()
        if self.ota is not None:
            self.ota.tick(now)
            if self.ota.finished:
                self.stop = True
                self.stop_code = self.ota.exit_code
        if self.log_mode and self.log_deadline and now >= self.log_deadline:
            self.log_mode = False
            self.log_deadline = 0.0
            self.reporter.line("日志透传结束")
        if self.interactive and now - self.last_stat >= 5.0:
            self.last_stat = now
            self.reporter.line(f"已转发 {self.reports} 帧报告，收到设备帧 {self.frames} 个，"
                               f"写回手柄 {self.outputs} 条")

    def run_interactive(self, read_stdin: bool = True) -> int:
        """交互会话主循环；图形界面传 read_stdin=False，命令由界面塞进队列。"""
        self.interactive = True
        self.forward = self.args.pad or not self.args.no_pad
        self.reporter.line("Remapad 会话已连接：不是 : 开头的行按固件 CLI 发送（:help 看工具命令）")
        if read_stdin:
            self.start_stdin()
        while not self.stop:
            now = time.monotonic()
            try:
                self.pump(now)
            except OSError as exc:
                self.reporter.error(f"链路错误：{exc}")
                self.reporter.event("link_error", message=str(exc))
                return 1
            time.sleep(0.001)
        return self.stop_code

    def query_once(self, command: str, hold_full_window: bool = False) -> bool:
        """发一条设备命令并等回复安静下来；收到任何行都返回 True。

        hold_full_window 用于应答不在 CLI 任务上、晚几拍才回的命令（mem 的
        报告由 PocketJS owner task 下一帧打印）：这类命令禁用安静窗提前退出，
        等满 --reply-wait，否则会话在 ok 之后、报告到达之前就退了。
        """
        self.link.purge_input()
        self.decoder = FrameDecoder()
        self.reply_seen = False
        deadline = time.monotonic() + self.args.reply_wait
        self.reply_deadline = deadline
        self.reply_hard_deadline = deadline
        self.send_cli(command)
        while not self.stop:
            now = time.monotonic()
            quiet_expired = self.reply_seen and now >= self.reply_deadline
            if now >= deadline or (quiet_expired and not hold_full_window):
                break
            self.pump(now)
            time.sleep(0.001)
        return self.reply_seen

    def run_command(self, command: str) -> int:
        """一次性命令：发一条、等回复、退出（桥接转发同时照跑）。"""
        self.one_shot = True
        self.expect_reply = True
        self.forward = self.args.pad and not self.args.no_pad
        # mem 的应答由 owner task 下一帧才打印，等满整个窗口再收尾。
        hold = command.split()[0] == "mem"
        if not self.query_once(command, hold_full_window=hold):
            self.reporter.error(f"{self.args.reply_wait:.1f} 秒内没有等到命令回复")
            return 1
        return 0

    def run_all(self) -> int:
        """一键拉取设备全部观测数据：数据全部由固件现场读取（不经过 UI 层，
        截图冻结或页面门控不影响实时性），每个 getter 发一条、等回复安静。"""
        self.one_shot = True
        self.expect_reply = True
        self.forward = self.args.pad and not self.args.no_pad
        missing = []
        for command in ALL_QUERIES:
            self.reporter.line(f"--- {command} " + "-" * max(0, 56 - len(command)))
            hold = command == "mem"  # mem 的应答下一帧才回，等满窗口。
            if not self.query_once(command, hold_full_window=hold):
                missing.append(command)
        if missing:
            self.reporter.error("没有回复的命令：" + " ".join(missing))
            return 1
        return 0

    def run_log(self, seconds: float, reset: bool, raw: bool) -> int:
        """只读日志：--reset 先脉冲复位，从启动日志开始读。"""
        self.forward = self.args.pad and not self.args.no_pad
        if reset:
            self.link.pulse_reset()
            self.line_buf = ""
            self.decoder = FrameDecoder()
        started = time.monotonic()
        deadline = started + seconds if seconds > 0 else 0.0
        self.log_mode = True
        self.log_raw = raw
        self.log_started = started
        while not self.stop:
            now = time.monotonic()
            if deadline and now >= deadline:
                break
            self.pump(now)
            time.sleep(0.001)
        return 0

    def run_shot(self, path: str | None) -> int:
        self.forward = self.args.pad and not self.args.no_pad
        self.request_shot(path)
        deadline = time.monotonic() + self.args.shot_timeout
        while not self.stop and not self.shot_ready:
            now = time.monotonic()
            if now >= deadline:
                self.reporter.error(f"{self.args.shot_timeout:.0f} 秒内没有收到完整截图")
                return 1
            self.pump(now)
            time.sleep(0.001)
        return 0 if self.shot_ready else 1

    def run_upgrade(self) -> int:
        """升级：状态机在 pump 里推进，桥接转发不断。"""
        self.forward = self.args.pad and not self.args.no_pad
        self.request_upgrade(None)
        while not self.stop:
            self.pump(time.monotonic())
            time.sleep(0.001)
        return self.stop_code


def print_local_help(reporter: Reporter) -> None:
    for text in (
        "本工具命令：",
        "  :help              显示这份清单",
        "  :all               拉取设备全部观测数据（status/mem/link/... 一键轮询）",
        "  :shot [路径]       抓实机截图并存成 PNG（默认 pc/shots/）",
        "  :log [秒|off]      透传设备日志（0 表示持续到 :log off）",
        "  :ota [镜像路径]    推固件镜像（默认 firmware/build/remapad_firmware.bin）",
        "  :quit              退出",
        "其余行按固件 CLI 原样发送。手柄功能的完整控制面都在固件 CLI 里：",
        "  输入注入 key/stick，身份 ctrl，连接 connect/pairing/wake/adv/drop，",
        "  上报内容 motion/headset/fwver/fwpost/fwack/fwapply，链路 ltk/relay，",
        "  反馈测试 rumble/lamp/haptic，屏幕 ui/backlight/screen，模式 mode。",
    ):
        reporter.line(text)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Remapad PC 侧单工具：桥接转发 + 串口命令行 + 实机截图 + OTA")
    parser.add_argument("-p", "--port", default="COM3", help="串口名（默认 COM3）")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USJ 忽略）")
    parser.add_argument("--vid", type=lambda value: int(value, 0), help="只挑该厂商 ID")
    parser.add_argument("--pid", type=lambda value: int(value, 0), help="只挑该产品 ID")
    parser.add_argument("--max-rate", type=float, default=250.0,
                        help="转发上限帧率（0 表示不限制，默认 250）")
    parser.add_argument("--no-rumble", action="store_true",
                        help="不把主机的震动/玩家灯写回手柄")
    parser.add_argument("--no-audio-haptics", action="store_true",
                        help="DS5 桥接时不走 PC 侧音频触觉（回落 HID 震动写回）")
    parser.add_argument("--no-pad", action="store_true",
                        help="任何模式都不转发手柄（只用命令行 / 截图 / 日志 / 升级）")
    parser.add_argument("--pad", action="store_true",
                        help="一次性命令 / 截图 / 日志 / 升级模式里也转发手柄（默认只转发交互模式）")
    parser.add_argument("--logs", action="store_true", help="同时打印设备日志文本")
    parser.add_argument("--reply-wait", type=float, default=1.2,
                        help="一次性命令等回复的秒数（默认 1.2）")
    parser.add_argument("--list", action="store_true", help="列出候选手柄接口后退出")
    parser.add_argument("--dump", action="store_true", help="只打印原始报告，不接串口")
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="--dump / --log 的时长（0 表示到 Ctrl+C）")
    parser.add_argument("--raw", action="store_true", help="--log 不加相对时间前缀")
    parser.add_argument("--reset", action="store_true", help="--log 先复位设备")
    parser.add_argument("--shot", action="store_true", help="抓实机截图后退出")
    parser.add_argument("--out", help="截图输出路径（默认 pc/shots/remapad-<时间戳>.png）")
    parser.add_argument("--shot-timeout", type=float, default=10.0,
                        help="等一次完整截图的秒数（默认 10）")
    parser.add_argument("--log", action="store_true", help="只读设备日志（--seconds 控制时长）")
    parser.add_argument("--all", action="store_true",
                        help="拉取设备全部观测数据（status/mem/link/... 逐条轮询）后退出")
    parser.add_argument("--upgrade", action="store_true", help="推固件镜像后重启设备")
    parser.add_argument("--image", default=DEFAULT_IMAGE,
                        help=f"镜像路径（默认 {DEFAULT_IMAGE}）")
    parser.add_argument("--dry-run", action="store_true", help="只校验镜像，不接设备")
    parser.add_argument("--wait", action="store_true",
                        help="--upgrade 后等设备重启回来并打印版本")
    parser.add_argument("--verbose", action="store_true", help="升级时透传设备日志")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="固件 CLI 命令（如 status），给了就执行一次后退出")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    """命令行入口：环境与镜像类问题统一映射成退出码 2（与改造前一致）。"""
    args = parse_args(argv)
    command = [item for item in args.command if item]
    try:
        return _run(args, command)
    except (HidUnavailable, ImageError) as exc:
        print(exc, file=sys.stderr)
        return 2


def _run(args, command) -> int:
    # 老用法的 log 子命令：等价于 --log，参数照旧。
    if command and command[0] == "log":
        args.log = True
        rest = command[1:]
        if "--reset" in rest:
            args.reset = True
        if "--raw" in rest:
            args.raw = True
        if "--seconds" in rest:
            index = rest.index("--seconds")
            if index + 1 < len(rest):
                args.seconds = float(rest[index + 1])
        command = []

    if args.dry_run:
        image_path = Path(args.image).expanduser()
        image, version = load_image(image_path)
        print(f"镜像 {image_path}：{len(image)} 字节，版本 {version}")
        print("dry-run：镜像校验通过，未连接设备")
        return 0

    if args.list:
        return run_list(load_hid())
    if args.dump:
        return run_dump(args, load_hid())

    hid = None if args.no_pad else load_hid()
    with open_port(args.port, args.baud) as ser:
        session = Session(args, hid, ser)
        try:
            if args.upgrade:
                code = session.run_upgrade()
            elif args.shot:
                code = session.run_shot(args.out)
            elif args.all:
                code = session.run_all()
            elif args.log:
                code = session.run_log(args.seconds, args.reset, args.raw)
            elif command:
                code = session.run_command(" ".join(command))
            else:
                code = session.run_interactive()
        except KeyboardInterrupt:
            print("\n已中断", file=sys.stderr)
            code = 130
        finally:
            session.detach_pad()
    if code != 0 or not args.wait or not args.upgrade:
        return code
    return wait_for_version(args.port, args.baud)


if __name__ == "__main__":
    raise SystemExit(main())

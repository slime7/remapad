#!/usr/bin/env python3
"""Remapad PC 侧单工具：桥接转发 + 串口命令行 + 实机截图 + 固件 OTA + amiibo 上传。

设备只有一根 Type-C：USB-Serial/JTAG 同时承载桥接帧、固件日志与 CLI 文本。
同一个进程持有这个口，因此转发手柄、敲命令、抓实机截图与推固件可以同时进行；
固件侧 input/input_link.c 按帧头分流，非帧字节交给 CLI 解析。

用法、交互命令与桥接帧格式见 pc/README.md 与 --help；常用入口：--list（枚举手柄）、
--dump（抓原始报告）、-p COMx（桥接 + 交互命令行）、--shot（实机截图）、--upgrade（OTA）、
--amiibo <bin>（上传镜像）。`:` 开头的是本工具命令（:help 看全表），其余行按固件 CLI 原样发送；
手柄转发默认只在交互模式里开，--pad / --no-pad 控制。图形界面入口见 remapadgui.py，
两边不要同时打开同一个串口。
"""

from __future__ import annotations

import argparse
import ctypes
import queue
import struct
import sys
import threading
import time
import zlib
from ctypes import wintypes
from pathlib import Path

from link import (
    AMIIBO_DATA_MAX,
    AMIIBO_NAME_MAX,
    AMIIBO_SIG_SIZE,
    AMIIBO_TAG_SIZE,
    CONN_BT,
    CONN_UNKNOWN,
    CONN_USB,
    FAMILY_NAMES,
    IMAGE_FORMAT_RGB565_LE,
    OTA_DATA_MAX,
    OTA_SLOT_WINDOW_END,
    OTA_WINDOW_FRAMES,
    TYPE_AMIIBO_ACK,
    TYPE_AMIIBO_BEGIN,
    TYPE_AMIIBO_DATA,
    TYPE_AMIIBO_END,
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
    TYPE_HOST_RAW,
    TYPE_PING,
    TYPE_REPORT,
    WIRE_MAX_PAYLOAD,
    FrameDecoder,
    SerialLink,
    amiibo_begin_payload,
    amiibo_data_payload,
    device_id,
    encode,
    family_for_vendor,
    feedback_params,
    open_port,
    ota_begin_payload,
    ota_data_payload,
    parse_amiibo_ack,
    parse_host_raw,
    parse_image_chunk,
    parse_image_end,
    parse_image_info,
    parse_ota_ack,
)

from ds5_haptics import Bt36OpusEncoder, Ds5HapticsAudio, Ds5HapticsBt

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


class AmiiboError(RuntimeError):
    """amiibo 文件不合法：不是 540 字节的 NTAG215 dump，或拿不出可用的名称。"""


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

AMIIBO_STATE_RECEIVING = 1
AMIIBO_STATE_DONE = 2
AMIIBO_STATE_FAILED = 3
#: ACK 里 slot 字段的「未落库」取值。
AMIIBO_SLOT_NONE = 0xFF

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


_MAX_DEVICE_ID_LEN = 200
_cfgmgr32 = ctypes.WinDLL("cfgmgr32", use_last_error=True)
_cfgmgr32.CM_Locate_DevNodeW.argtypes = [
    ctypes.POINTER(wintypes.DWORD), wintypes.LPCWSTR, wintypes.ULONG]
_cfgmgr32.CM_Get_Parent.argtypes = [
    ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, wintypes.ULONG]
_cfgmgr32.CM_Get_Device_IDW.argtypes = [
    wintypes.DWORD, wintypes.LPWSTR, wintypes.ULONG, wintypes.ULONG]


def instance_id_of_hid_path(path: str | bytes) -> str | None:
    r"""hidapi 设备接口路径 → 设备树实例 ID。

    路径形如 \\?\HID#VID_054C&PID_0DF2&MI_03#8&2f3d4f&0&0000#{接口 GUID}，
    实例 ID 是前三段的 # 换 \：HID\VID_054C&PID_0DF2&MI_03\8&2f3d4f&0&0000。
    hidapi 给的 path 是 bytes，先按 UTF-8 解码。
    """
    if isinstance(path, bytes):
        path = path.decode("utf-8", "replace")
    if not path.startswith("\\\\?\\"):
        return None
    parts = path[4:].split("#")
    # 末段是接口 GUID，缺它就不是完整的设备接口路径。
    if len(parts) < 4 or not all(parts[:3]):
        return None
    return "\\".join(parts[:3])


def is_virtual_pad(path: str) -> bool:
    """按设备树判断是不是 ViGEm 之类的虚拟手柄。

    Moonlight/Sunshine 串流时会在主机上虚拟一块 DS4（ViGEmBus 总线），hidapi
    把它枚举成普通 USB 手柄，按顺序选柄会把它当桥接目标抓走——输入转发与震动
    写回全进虚拟设备，真手柄反而时有时无。hidapi 的路径里看不出虚拟与否，这里
    沿设备树向上查祖先的设备 ID，任一代以 VIGEM 开头即虚拟；查不到实例或祖先
    （非常规设备树）一律按真实手柄处理，绝不静默丢设备。
    """
    instance = instance_id_of_hid_path(path)
    if instance is None:
        return False
    devinst = wintypes.DWORD()
    if _cfgmgr32.CM_Locate_DevNodeW(ctypes.byref(devinst), instance, 0) != 0:
        return False
    for _ in range(8):
        parent = wintypes.DWORD()
        if _cfgmgr32.CM_Get_Parent(ctypes.byref(parent), devinst, 0) != 0:
            return False
        device_id = ctypes.create_unicode_buffer(_MAX_DEVICE_ID_LEN)
        if _cfgmgr32.CM_Get_Device_IDW(parent, device_id, _MAX_DEVICE_ID_LEN, 0) != 0:
            return False
        if device_id.value.upper().startswith("VIGEM"):
            return True
        devinst = parent
    return False


def list_candidates(hid) -> list[dict]:
    """枚举候选手柄接口：同一只手柄可能有多个 HID 接口，这里只留手柄用途的；
    ViGEm 之类的虚拟手柄排除在外（见 partition_virtual）。"""
    real, _virtual = partition_virtual(_gamepad_usages(hid))
    return real


def list_virtual_pads(hid) -> list[dict]:
    """被候选清单排除的虚拟手柄（--list 里标注展示）。"""
    return partition_virtual(_gamepad_usages(hid))[1]


def _gamepad_usages(hid):
    """枚举手柄用途的 HID 接口（Generic Desktop / Joystick 与 Game Pad）。"""
    for info in hid.enumerate():
        if info.get("usage_page", 0) != GAMEPAD_USAGE_PAGE:
            continue
        if info.get("usage", 0) in GAMEPAD_USAGES:
            yield info


def partition_virtual(infos, is_virtual=None) -> tuple[list[dict], list[dict]]:
    """按设备树把候选拆成真实与虚拟两份（is_virtual 可注入便于测试）。"""
    if is_virtual is None:
        is_virtual = is_virtual_pad
    real: list[dict] = []
    virtual: list[dict] = []
    for info in infos:
        if is_virtual(info["path"]):
            virtual.append(info)
        else:
            real.append(info)
    return real, virtual


def conn_for(info: dict) -> int:
    bus = info.get("bus_type")
    if bus == 1:
        return CONN_USB
    if bus == 2:
        return CONN_BT
    return CONN_UNKNOWN


def bt_haptics_wanted(args) -> bool:
    """蓝牙接入的 DualSense 是否启用私有触觉流（0x32/0x36）。

    默认启用：DS5 的 HD 触觉与手柄喇叭只有这条流承载，0x31 的 HID 写回只剩两带
    震动（发声段直接丢）。--no-bt-haptics 显式关掉回落 HID 写回；写回被驱动拒绝
    时也会自动回落（见 Session._lose_haptics）。
    """
    return not getattr(args, "no_bt_haptics", False)


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
    hd = params.get("hd")
    if hd is not None:
        # HD 时序子帧段：固件按布局行重整出的子帧序列（PC 侧只做哑渲染）。
        line += (f" HD {hd['l']['count']}+{hd['r']['count']}子帧"
                 + (" 发声" if hd["speaker"][1] else ""))
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


class WriteBackGate:
    """桥接写回限速：游戏内震动包络逐包都变，设备侧「字节变了才发」压不住
    写回量，蓝牙 HID 写回又慢，会把会话循环拖到输入转发卡顿。把写回钉在
    min_interval_s 上限：
    窗口内只放行第一条，被挡下的帧不丢、留作最新待写帧，窗口到期由 poll
    放行——收尾状态（比如停震的最后一帧）因此一定落地，马达不会被钉住。"""

    def __init__(self, min_interval_s: float = 0.03) -> None:
        self._min = min_interval_s
        self._next_ok = 0.0
        self._pending: bytes | None = None

    def admit(self, payload: bytes, now: float) -> bool:
        """会话循环收到 OUT_REPORT 时调用：True 表示这一帧现在就写。"""
        if not payload:
            return False
        if now >= self._next_ok:
            self._next_ok = now + self._min
            self._pending = None
            return True
        self._pending = payload
        return False

    def poll(self, now: float) -> bytes | None:
        """窗口到期后放行最新待写帧；会话循环每轮调用一次。"""
        if self._pending is not None and now >= self._next_ok:
            payload, self._pending = self._pending, None
            self._next_ok = now + self._min
            return payload
        return None


def run_list(hid) -> int:
    candidates = list_candidates(hid)
    virtual = list_virtual_pads(hid)
    if not candidates and not virtual:
        print("没有找到手柄接口")
        return 1
    for info in candidates:
        print(describe(info))
    for info in virtual:
        print(describe(info) + "（虚拟手柄，不参与转发）")
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


class HostCaptureSink:
    """主机原始输出采集的落盘器：一行一条记录（相对时间、通道、十六进制字节）。

    数据是固件在解析与布局转换之前收到的主机输出（震动参数包、指令帧、
    复合输出、固件更新记录流），经桥接帧 0x12 到这里。帧头 slot 是设备侧
    记录号：跳号说明设备队列满、丢过包，按次数汇总不逐条打断。
    """

    def __init__(self, path: Path, started: float) -> None:
        self.path = path
        self.started = started
        self.count = 0
        self.gaps = 0
        self._file = None
        self._last_seq: int | None = None

    def open(self) -> None:
        if self.path.parent != Path(""):
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file = self.path.open("w", encoding="utf-8", newline="\n")
        self._file.write(
            f"# remapad host raw capture {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
            "# 列：<相对秒> <通道>[句柄] seq=<记录号> <字节数>B[ trunc] <十六进制字节>\n")

    def write_record(self, parsed: dict, seq: int, now: float) -> None:
        if self._file is None:
            return
        if self._last_seq is not None and seq != ((self._last_seq + 1) & 0xFF):
            self.gaps += 1
        self._last_seq = seq
        self.count += 1
        data = parsed["data"]
        marker = " trunc" if parsed["truncated"] else ""
        self._file.write(
            f"{now - self.started:+8.3f}s {parsed['name']}[0x{parsed['channel']:02X}] "
            f"seq={seq:03d} {len(data):3d}B{marker} {data.hex(' ')}\n")

    def close(self) -> str | None:
        """收口落盘器，返回总结一行（本来就没开过返回 None）。"""
        if self._file is None:
            return None
        self._file.close()
        self._file = None
        summary = f"采集已保存：{self.path}（{self.count} 条"
        if self.gaps:
            summary += f"，{self.gaps} 处跳号（设备队列满丢包）"
        return summary + "）"


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


class AmiiboJob:
    """amiibo 上传状态机：BEGIN → 一段 DATA → END，逐帧 ACK 驱动；由会话
    主循环 tick 驱动，桥接转发同时照跑。镜像固定 540 字节（三个数据帧），
    设备逐帧回 ACK，received 就是续传起点。"""

    ACK_TIMEOUT_S = 3.0
    MAX_RETRIES = 5

    def __init__(self, name: str, data: bytes, send, reporter: Reporter) -> None:
        self.name = name
        self.data = data
        self._send = send
        self.reporter = reporter
        self.received = 0
        self.retries = 0
        self.phase = "begin"
        self.deadline = 0.0
        self.finished = False
        self.exit_code = 1

    def start(self, now: float) -> None:
        self.reporter.line(f"上传 amiibo「{self.name}」（{len(self.data)} 字节）")
        self.reporter.event("amiibo_started", amiibo=self.name, size=len(self.data))
        self._send(encode(TYPE_AMIIBO_BEGIN, 0, 0,
                          amiibo_begin_payload(self.name, len(self.data))))
        self.phase = "begin"
        self.deadline = now + self.ACK_TIMEOUT_S

    def _send_pending(self, now: float) -> None:
        """从设备已确认的字节起重发剩余数据；设备对已收区间按幂等处理。"""
        window = bytearray()
        offset = self.received
        while offset < len(self.data):
            piece = self.data[offset:offset + AMIIBO_DATA_MAX]
            window += encode(TYPE_AMIIBO_DATA, 0, 0,
                             amiibo_data_payload(offset, piece), max_payload=WIRE_MAX_PAYLOAD)
            offset += len(piece)
        self._send(bytes(window))
        self.deadline = now + self.ACK_TIMEOUT_S

    def tick(self, now: float) -> None:
        if self.finished or now < self.deadline:
            return
        if self.phase == "begin":
            self._fail("设备没有回应上传请求；确认设备是 COM 模式、串口没被占用"
                       "且固件已带 amiibo 功能")
            return
        if self.phase == "data":
            self.retries += 1
            if self.retries > self.MAX_RETRIES:
                self._fail(f"连续 {self.retries} 次没有等到数据应答，上传中止；"
                           "设备侧 5 秒无数据也会自行作废会话")
                return
            self.reporter.line(f"数据应答超时，从 {self.received} 字节处重发（第 {self.retries} 次）")
            self._send_pending(now)
            return
        if self.phase == "end":
            self._fail("设备没有确认收尾，槽位没有落库")

    def on_ack(self, ack: dict) -> None:
        if self.finished:
            return
        if self.phase == "begin":
            if ack["state_id"] != AMIIBO_STATE_RECEIVING or ack["code_id"] != 0:
                self._fail(f"设备拒绝上传（{describe_amiibo_ack(ack)}）")
                return
            self.phase = "data"
            self._send_pending(time.monotonic())
            return
        if self.phase == "data":
            if ack["state_id"] == AMIIBO_STATE_FAILED:
                self._fail(f"设备中止上传（{describe_amiibo_ack(ack)}），"
                           f"已确认 {ack['received']} 字节")
                return
            self.retries = 0
            self.received = ack["received"]
            if self.received >= len(self.data):
                self.phase = "end"
                self._send(encode(TYPE_AMIIBO_END, 0, 0))
                self.deadline = time.monotonic() + self.ACK_TIMEOUT_S
            return
        if self.phase == "end":
            if ack["state_id"] == AMIIBO_STATE_DONE and ack["code_id"] == 0:
                self.finished = True
                self.exit_code = 0
                slot = ack["slot"]
                self.reporter.line(f"上传完成：「{self.name}」已存为槽位 "
                                   f"{'-' if slot == AMIIBO_SLOT_NONE else slot}")
                self.reporter.event("amiibo_finished", ok=True, slot=slot, amiibo=self.name)
            else:
                self._fail(f"设备落库失败（{describe_amiibo_ack(ack)}）")

    def _fail(self, message: str) -> None:
        self.reporter.error(message)
        self.reporter.event("amiibo_finished", ok=False, message=message)
        self.finished = True
        self.exit_code = 1


def describe_amiibo_ack(ack: dict) -> str:
    return f"state={ack['state']} code={ack['code']} received={ack['received']}"


def load_amiibo(path: Path) -> tuple[str, bytes]:
    """读入 amiibo dump（540 字节 NTAG215 镜像，或 572 字节镜像 + 厂商签名），
    返回（名称, 字节）。572 的签名段进设备读缓冲头区，主机校验签名时必需。

    名称取文件名主干，按 UTF-8 截到 31 字节（不在多字节字符中间截断），
    是设备侧槽位的显示名；不合法抛 AmiiboError。
    """
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise AmiiboError(f"读不到 amiibo 文件 {path}：{exc}") from exc
    if len(data) not in (AMIIBO_TAG_SIZE, AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE):
        raise AmiiboError(
            f"{path} 是 {len(data)} 字节，amiibo dump 固定是 {AMIIBO_TAG_SIZE}（纯镜像）"
            f"或 {AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE}（镜像 + 厂商签名）字节")
    name = path.stem.encode("utf-8")[:AMIIBO_NAME_MAX].decode("utf-8", errors="ignore").strip()
    if not name:
        raise AmiiboError(f"{path} 的文件名拿不出可用的槽位名")
    return name, data


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


def _reply_fields(line: str) -> dict[str, str]:
    """把一行回读按空白切开，收下所有 `键=值` 片段；值里带空格时只留第一段。"""
    fields: dict[str, str] = {}
    for token in line.split():
        key, sep, value = token.partition("=")
        if sep and key and key not in fields:
            fields[key] = value
    return fields


def _int_field(text: str | None) -> int | None:
    if not text:
        return None
    try:
        return int(text, 10)
    except ValueError:
        return None


def _hex_field(text: str | None) -> int | None:
    """`0xRRGGBB` → 整数；不是合法的一段配色返回 None（界面对应项保持原值）。"""
    if not text:
        return None
    try:
        value = int(text, 16)
    except ValueError:
        return None
    return value if 0 <= value <= 0xFFFFFF else None


def _battery_field(text: str | None) -> tuple[int, int] | None:
    """`3971mV/85%` → （毫伏, 百分比）。"""
    if not text or "/" not in text:
        return None
    millivolts, _, percent = text.partition("/")
    if millivolts.endswith("mV"):
        millivolts = millivolts[:-2]
    if percent.endswith("%"):
        percent = percent[:-1]
    mv, pct = _int_field(millivolts), _int_field(percent)
    return (mv, pct) if mv is not None and pct is not None else None


def parse_device_reply(line: str) -> tuple[str, dict] | None:
    """识别固件设置回读行 → （通道, 字段）；认不出的行返回 None。

    图形界面用它把设备回读同步进设置控件：固件是唯一事实源，界面不自算状态。
    通道与字段：

    - `backlight`：`backlight 60` → `light`；
    - `screen`：`screen on|off` → `screen_on`；
    - `ctrl`：`ok ctrl body=0x… button=0x… accent=0x… grip=0x…` → 四段 `0xRRGGBB`；
    - `ds`：`ds touchpad=on|off capture=on|off` → `touchpad_plus_minus` / `capture_key`；
    - `device`：`status` 与 `version` 的一行回读 → 版本、分区、电池、堆与配对等事实，
      固件字段名收敛成 `firmware` / `partition` / `image` / `ota_state` /
      `pairing` / `role` / `pad` / `light` / `screen_on` / `battery_mv` /
      `battery_percent` / `charging` / `heap` / `uptime_s`。

    带状态词的应答先剥前缀：`err` 行大多答不进这里的字段表，自然返回 None。
    """
    text = line.strip()
    for status in ("ok ", "err "):
        if text.startswith(status):
            text = text[len(status):].strip()
            break
    if not text:
        return None
    words = text.split()
    if words[0] == "backlight" and len(words) > 1:
        light = _int_field(words[1])
        if light is not None and 0 <= light <= 100:
            return "backlight", {"light": light}
        return None
    if words[0] == "screen" and len(words) > 1:
        if words[1] in ("on", "off"):
            return "screen", {"screen_on": words[1] == "on"}
        return None
    fields = _reply_fields(text)
    if words[0] == "ctrl" and all(key in fields for key in ("body", "button", "accent", "grip")):
        colors = {key: _hex_field(fields[key]) for key in ("body", "button", "accent", "grip")}
        if all(value is not None for value in colors.values()):
            return "ctrl", colors
        return None
    if words[0] == "ds":
        touchpad = fields.get("touchpad")
        capture = fields.get("capture")
        if touchpad in ("on", "off") and capture in ("on", "off"):
            return "ds", {"touchpad_plus_minus": touchpad == "on",
                          "capture_key": capture == "on"}
        return None
    if words[0] == "state" or text.startswith("fw="):
        facts: dict[str, object] = {}
        for key, name in (("fw", "firmware"), ("part", "partition"), ("image", "image"),
                          ("ota", "ota_state"), ("pairing", "pairing"), ("role", "role"),
                          ("pad", "pad")):
            if key in fields:
                facts[name] = fields[key]
        for key, name in (("backlight", "light"), ("heap", "heap")):
            value = _int_field(fields.get(key))
            if value is not None:
                facts[name] = value
        if fields.get("screen") in ("0", "1"):
            facts["screen_on"] = fields["screen"] == "1"
        if fields.get("chg") in ("0", "1"):
            facts["charging"] = fields["chg"] == "1"
        uptime = _int_field((fields.get("uptime") or "").removesuffix("s"))
        if uptime is not None:
            facts["uptime_s"] = uptime
        battery = _battery_field(fields.get("batt"))
        if battery is not None:
            facts["battery_mv"], facts["battery_percent"] = battery
        return ("device", facts) if facts else None
    return None


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
        self.amiibo: AmiiboJob | None = None
        # 主机原始输出采集（:capture / --capture 开启）：落盘器挂在会话上。
        self.capture: HostCaptureSink | None = None
        self.captured = 0
        self.stop = False
        self.stop_code = 0
        #: 0x31 写回的短写计数（hidapi 返回值 < 报告长度 = 驱动没收下）。
        self.writeback_short = 0
        # 反馈帧打印限频（不影响写回手柄，见 FeedbackThrottle）。
        self.feedback_gate = FeedbackThrottle()
        # 写回手柄的限速门（蓝牙 HID 写回慢，见 WriteBackGate）。
        self.write_gate = WriteBackGate()
        # DS5 桥接时的 PC 侧音频触觉（attach 时按需启动）。
        self.haptics: Ds5HapticsAudio | None = None
        self._haptics_starting = False
        # 音频流开好后待发的「haptic audio on」：串口只允许主循环一个写者，
        # 后台线程只置这个标志。
        self._haptics_notify = False
        # 触觉流写回失败（后台线程置位）：主循环收尾并回落 HID 震动写回。
        self._haptics_lost = False

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
        """DS5 接入时启用 PC 侧音频触觉：USB 直插走 4ch 音频端点（频道 3/4
        触觉、1/2 发声）。蓝牙接入默认启用私有触觉流（0x36 HD 触觉 + 手柄
        喇叭真声，无 PyAV/libopus 时回落 0x32 音圈）——DS5 的 HD 触觉与手柄
        喇叭只有这条流承载，HID 的 0x31 只剩两带震动；--no-bt-haptics 关掉
        回落 HID 写回，写回被驱动拒绝时也会自动回落。
        --no-audio-haptics / --no-rumble 或通路开不起来时静默回落 HID 震动。
        开流要秒级、且不能占着桥接热路径，启动放后台线程。"""
        if self.haptics is not None or self._haptics_starting:
            return
        if getattr(self.args, "no_audio_haptics", False) or self.args.no_rumble:
            return
        info = self.pad_info or {}
        if (info.get("vendor_id") != 0x054C
                or info.get("product_id") not in (0x0CE6, 0x0DF2)):
            return
        if conn_for(info) == CONN_BT:
            if not bt_haptics_wanted(self.args):
                self.reporter.line(
                    "蓝牙接入：--no-bt-haptics 保持 HID 震动写回"
                    "（HD 触觉与手柄喇叭不启用）")
                return
            self._haptics_starting = True
            threading.Thread(target=self._start_bt_haptics_worker, daemon=True).start()
            return
        if conn_for(info) != CONN_USB:
            self._haptics_starting = False
            return
        threading.Thread(target=self._start_haptics_worker, daemon=True).start()

    def _start_haptics_worker(self) -> None:
        audio = Ds5HapticsAudio(self.reporter)
        if not audio.start():
            self._haptics_starting = False
            return
        self._adopt_haptics(audio)

    def _start_bt_haptics_worker(self) -> None:
        # 0x32 音圈流兜底；PyAV/libopus 可用时升级 0x36（HD 触觉 + 手柄喇叭
        # 真声），编码器建不起来就按 0x32 走。
        try:
            encoder = Bt36OpusEncoder()
        except Exception:  # noqa: BLE001 - 缺依赖/无 libopus 都按回落处理
            encoder = None
        audio = Ds5HapticsBt(self.pad, self.reporter, on_error=self._lose_haptics,
                             speaker_encoder=encoder)
        if not audio.start():
            self._haptics_starting = False
            return
        self._adopt_haptics(audio)

    def _lose_haptics(self, _exc) -> None:
        """触觉流写回失败（后台线程调用）：主循环收尾回落 HID 震动。"""
        self._haptics_lost = True

    def _adopt_haptics(self, audio) -> None:
        if self.pad is None:
            # 启动期间手柄已断开（会话收尾）：不留孤儿流。
            audio.stop()
            self._haptics_starting = False
            return
        self.haptics = audio
        self._haptics_starting = False
        # 「haptic audio on」由主循环发：串口句柄不跨线程写（并发写会把命令字节冲烂）。
        # 让位要等私有流真的接到 HD 子帧（见 pump_haptics_notify）。
        self._haptics_notify = False

    def pump_haptics_notify(self) -> None:
        if self._haptics_lost:
            self._haptics_lost = False
            if self.haptics is not None:
                self.reporter.error("触觉流写回被拒，回落 HID 震动写回")
                self.stop_haptics()
        if self.haptics is None or self._haptics_notify:
            return
        # 让位（haptic audio on）等私有流真的接到 HD 子帧之后再发：没接到内容
        # 的通路不驱动音圈（老固件、布局行没声明 HD 通路），提前让位会把手柄
        # 留在「HID 震动已清零、音频也没有内容」的静默状态。
        if not getattr(self.haptics, "engaged", False):
            return
        self._haptics_notify = True
        try:
            self.send_cli("haptic audio on")
            self.reporter.line(getattr(self.haptics, "label", None)
                               or self.haptics.LABEL)
            self.reporter.event("haptics_audio", state="on")
        except OSError:
            self.stop_haptics()

    def stop_haptics(self) -> None:
        if self.haptics is None:
            return
        stats = getattr(self.haptics, "stats", None)
        self.haptics.stop()
        if stats is not None:
            self.reporter.line(stats())
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
        """把设备编码好的输出报告写回手柄：布局知识只在固件里有一份。
        写回经 WriteBackGate 限速——蓝牙 HID 写回慢，逐条写会把会话循环
        拖到输入转发卡顿；被挡下的帧由 pump 在窗口到期后放行最新一帧。"""
        if self.args.no_rumble or self.pad is None or not payload:
            return False
        if not self.write_gate.admit(payload, time.monotonic()):
            return False
        return self.send_output_report(payload)

    def send_output_report(self, payload: bytes) -> bool:
        try:
            written = self.pad.write(payload)
        except OSError as exc:
            self.reporter.error(f"反馈写回失败：{exc}")
            return False
        # hidapi 用返回值报「实际交给驱动的字节数」。Windows 后端会把短于
        # 描述符声明长度的写回补齐到 OutputReportByteLength 再交驱动（DS5 蓝牙
        # 集合声明 547，0x31 的 78 字节写法因此返回 547）——返回值比载荷长是
        # 常态，只有真的少交（< 载荷长度）才是这一份没写进去（LED/震动写回
        # 静默丢失，只捕异常看不出来）。
        if isinstance(written, int) and written < len(payload):
            self.writeback_short += 1
            if self.writeback_short == 1 or self.writeback_short % 200 == 0:
                self.reporter.error(f"反馈写回短写：{written}/{len(payload)} 字节"
                                    f"（累计 {self.writeback_short} 份）")
            return False
        return True

    # --- 链路读取 --------------------------------------------------

    def pump_link(self, now: float) -> None:
        chunk = self.link.read()
        if not chunk:
            return
        frames, text = self.decoder.feed(chunk)
        for frame_type, slot, _seq, payload in frames:
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
            elif frame_type == TYPE_HOST_RAW:
                # 主机输出的原始采集：有落盘器写文件，没有就只计数
                # （设备端 `capture on` 后必须用 :capture <路径> 接住才有文件）。
                self.captured += 1
                if self.capture is not None:
                    self.capture.write_record(parse_host_raw(payload), slot, now)
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
            elif frame_type == TYPE_AMIIBO_ACK and self.amiibo is not None:
                self.amiibo.on_ack(parse_amiibo_ack(payload))
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
        elif name == "amiibo":
            if not arguments:
                self.reporter.error("用法：:amiibo <bin 文件路径>（540 字节 NTAG215 dump）")
                return
            try:
                self.request_amiibo_upload(arguments)
            except AmiiboError as exc:
                self.reporter.error(str(exc))
        elif name == "capture":
            self.run_capture_command(arguments)
        elif name in ("quit", "q", "exit"):
            self.stop = True
        else:
            self.reporter.error(f"未知的工具命令：{name}（:help 看清单）")

    def run_capture_command(self, arguments: str) -> None:
        """:capture <路径> 开始落盘、:capture off 停止、无参看状态。"""
        if arguments == "off":
            if self.capture is None:
                self.reporter.line("主机原始输出采集未开启")
            else:
                self.stop_capture()
            return
        if not arguments:
            if self.capture is None:
                self.reporter.line("主机原始输出采集未开启（:capture <文件路径> 开始，"
                                   "文件存主机写进手柄/设备之前的原始字节）")
            else:
                self.reporter.line(f"采集中 → {self.capture.path}（已落盘 "
                                   f"{self.capture.count} 条；:capture off 停止）")
            return
        if self.capture is not None:
            self.stop_capture()
        self.start_capture(arguments)

    def start_capture(self, path_str: str) -> None:
        """开一个采集落盘器并让固件开始上行（设备命令 capture on）。"""
        try:
            sink = HostCaptureSink(Path(path_str).expanduser(), time.monotonic())
            sink.open()
        except OSError as exc:
            self.reporter.error(f"打不开采集文件：{exc}")
            return
        self.capture = sink
        try:
            self.send_cli("capture on")
        except OSError:
            self.capture = None
            sink.close()
            raise
        self.reporter.line(f"主机原始输出采集已开启 → {sink.path}"
                           f"（震动/玩家灯/指令等主机输出的原始字节，:capture off 停止）")
        self.reporter.event("capture_started", path=str(sink.path))

    def stop_capture(self) -> None:
        if self.capture is None:
            return
        sink, self.capture = self.capture, None
        try:
            self.send_cli("capture off")
        except OSError:
            pass
        summary = sink.close()
        if summary:
            self.reporter.line(summary)
        self.reporter.event("capture_stopped", count=sink.count)

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

    def request_amiibo_upload(self, amiibo_path: str) -> None:
        path = Path(amiibo_path).expanduser()
        name, data = load_amiibo(path)
        self.amiibo = AmiiboJob(name, data, self.link.write, self.reporter)
        self.amiibo.start(time.monotonic())

    # --- 主循环 ----------------------------------------------------

    def pump(self, now: float) -> None:
        self.pump_pad(now)
        self.pump_link(now)
        self.pump_commands()
        self.pump_haptics_notify()
        # 写回限速门的待写帧放行：窗口到期后把最新一帧补写出去，
        # 停震的收尾帧不因限速丢失。
        pending = self.write_gate.poll(now)
        if pending is not None and self.pad is not None and not self.args.no_rumble:
            if self.send_output_report(pending):
                self.outputs += 1
        if self.ota is not None:
            self.ota.tick(now)
            if self.ota.finished:
                self.stop = True
                self.stop_code = self.ota.exit_code
        if self.amiibo is not None:
            self.amiibo.tick(now)
            if self.amiibo.finished:
                code = self.amiibo.exit_code
                self.amiibo = None
                # 交互模式的 :amiibo 不退出会话（传完可以接着 select / list）；
                # 一次性 --amiibo 在这里收口退出。
                if not self.interactive:
                    self.stop = True
                    self.stop_code = code
        if self.log_mode and self.log_deadline and now >= self.log_deadline:
            self.log_mode = False
            self.log_deadline = 0.0
            self.reporter.line("日志透传结束")
        if self.interactive and now - self.last_stat >= 5.0:
            self.last_stat = now
            stats = (f"已转发 {self.reports} 帧报告，收到设备帧 {self.frames} 个，"
                     f"写回手柄 {self.outputs} 条")
            if self.capture is not None:
                stats += f"，采集已落盘 {self.capture.count} 条"
            elif self.captured:
                stats += f"，收到采集帧 {self.captured} 个（:capture <路径> 落盘）"
            self.reporter.line(stats)

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

    def run_amiibo(self, path: str) -> int:
        """上传 amiibo：状态机在 pump 里推进，完成即退出。"""
        self.forward = self.args.pad and not self.args.no_pad
        self.request_amiibo_upload(path)
        while self.amiibo is not None and not self.stop:
            self.pump(time.monotonic())
            time.sleep(0.001)
        return self.stop_code

    def run_capture(self, path: str, seconds: float) -> int:
        """抓主机原始输出到文件：--seconds 控制时长（0 = 到 Ctrl+C），
        手柄转发照常（--pad / 交互默认开）——实体手柄连着串口时同样能抓。"""
        self.forward = self.args.pad and not self.args.no_pad
        self.start_capture(path)
        deadline = time.monotonic() + seconds if seconds > 0 else 0.0
        while not self.stop:
            now = time.monotonic()
            if deadline and now >= deadline:
                break
            self.pump(now)
            time.sleep(0.001)
        self.stop_capture()
        return 0


def print_local_help(reporter: Reporter) -> None:
    for text in (
        "本工具命令：",
        "  :help              显示这份清单",
        "  :all               拉取设备全部观测数据（status/mem/link/... 一键轮询）",
        "  :shot [路径]       抓实机截图并存成 PNG（默认 pc/shots/）",
        "  :log [秒|off]      透传设备日志（0 表示持续到 :log off）",
        "  :ota [镜像路径]    推固件镜像（默认 firmware/build/remapad_firmware.bin）",
        "  :amiibo <bin 路径> 上传 amiibo 镜像到设备（之后用固件命令 amiibo select 选用）",
        "  :capture [路径|off] 抓主机原始输出到文件（震动/玩家灯/指令，布局转换前）",
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
    parser.add_argument("--bt-haptics", action="store_true",
                        help="蓝牙接入的 DS5 启用私有触觉流（默认已启用，保留作显式声明："
                             "0x36 HD 触觉 + 手柄喇叭，无 PyAV 时回落 0x32 音圈）")
    parser.add_argument("--no-bt-haptics", action="store_true",
                        help="蓝牙接入的 DS5 不用私有触觉流，回落 0x31 HID 两带震动"
                             "（HD 触觉与手柄喇叭都不启用）")
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
    parser.add_argument("--capture", metavar="FILE",
                        help="抓主机原始输出（震动/玩家灯/指令，布局转换前）到文件后退出"
                             "（--seconds 控制时长，0 = 到 Ctrl+C；--pad 可同时转发手柄）")
    parser.add_argument("--all", action="store_true",
                        help="拉取设备全部观测数据（status/mem/link/... 逐条轮询）后退出")
    parser.add_argument("--upgrade", action="store_true", help="推固件镜像后重启设备")
    parser.add_argument("--image", default=DEFAULT_IMAGE,
                        help=f"镜像路径（默认 {DEFAULT_IMAGE}）")
    parser.add_argument("--amiibo", metavar="BIN",
                        help="上传 amiibo 镜像（540 字节 NTAG215 dump）到设备后退出")
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
    except (HidUnavailable, ImageError, AmiiboError) as exc:
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
            elif args.amiibo:
                code = session.run_amiibo(args.amiibo)
            elif args.shot:
                code = session.run_shot(args.out)
            elif args.all:
                code = session.run_all()
            elif args.log:
                code = session.run_log(args.seconds, args.reset, args.raw)
            elif args.capture:
                code = session.run_capture(args.capture, args.seconds)
            elif command:
                code = session.run_command(" ".join(command))
            else:
                code = session.run_interactive()
        except KeyboardInterrupt:
            print("\n已中断", file=sys.stderr)
            code = 130
        finally:
            session.stop_capture()
            session.detach_pad()
    if code != 0 or not args.wait or not args.upgrade:
        return code
    return wait_for_version(args.port, args.baud)


if __name__ == "__main__":
    raise SystemExit(main())

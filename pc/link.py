#!/usr/bin/env python3
"""Remapad 桥接链路的 PC 侧：免复位串口打开 + 桥接帧编解码。

串口打开是全仓库 PC 侧工具的唯一实现（remapadctl.py 的转发、命令行、截图与 OTA 共用）：
直接用 Win32 API，并在打开前后把 DTR/RTS 固定为低电平——USB-Serial/JTAG 的
片内状态机把这两条线当复位控制线解释（RTS 拉高即复位），普通串口库默认会在
打开端口时拉起它们。

帧格式与固件侧 input_frame.c 一致：
    A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
CRC-16/CCITT-FALSE 覆盖除末尾两字节外的整帧（含同步字）。

同一套帧格式还承载 OTA 升级（固件侧 ota_proto.c）：类型 0x30-0x33，数据帧
载荷到 202 字节，因此编码入口允许显式放宽载荷上限（OTA_MAX_PAYLOAD）。

串口枚举（list_serial_ports）读 HKLM 的 SERIALCOMM 键，不引入 pyserial；
打开失败的提示文案由 open_hint 统一给出，命令行与图形界面共用一份。
"""

from __future__ import annotations

import ctypes
import struct
import sys
import time
from ctypes import wintypes

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（各 PC 端工具同一做法）。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

if sys.platform != "win32":
    print("pc/link.py 目前只有 Windows 实现（Win32 串口 API）。", file=sys.stderr)
    raise SystemExit(2)

SYNC0 = 0xA5
SYNC1 = 0x5A
VERSION = 0x01
HEADER_LEN = 7
CRC_LEN = 2
#: PC → 设备报文帧的载荷上限：8 字节设备标识 + 单帧最多 64 字节原始报告。
MAX_PAYLOAD = 72
MAX_FRAME = HEADER_LEN + MAX_PAYLOAD + CRC_LEN
#: 设备 → PC 输出报告帧的载荷上限：DualSense / DualShock 4 蓝牙输出报告各
#: 78 字节（Report ID + 77 字节字段），比报文帧大一档。
OUT_REPORT_MAX = 78
#: 线格式上限：帧头里的长度字段是单字节，OTA 数据帧用到 202 字节。
WIRE_MAX_PAYLOAD = 255
#: 解码器接受的单帧载荷上限：取线格式上限。截图分块帧的载荷是 4 字节偏移 +
#: 200 字节像素，比输出报告帧大一档；CRC 仍然逐帧校验，放宽上限只是让这类
#: 长载荷帧能被收下。
DECODE_MAX_PAYLOAD = WIRE_MAX_PAYLOAD
#: 截图声明载荷：宽 u16 LE + 高 u16 LE + 格式 u8。
IMAGE_INFO_LEN = 5
#: 像素格式 1 = RGB565 小端：固件把渲染缓冲原样回传，不换字节序。
IMAGE_FORMAT_RGB565_LE = 1
#: 截图分块载荷：偏移 u32 LE + 像素数据。
IMAGE_OFF_LEN = 4

TYPE_ATTACH = 0x01
TYPE_DETACH = 0x02
TYPE_REPORT = 0x10
#: 设备 → PC：要写回手柄的输出报告（原始字节，首字节是 Report ID）。
TYPE_OUT_REPORT = 0x11
#: 设备 → PC：主机输出的原始采集（通道 + 标志/长度 + 原始字节，布局转换
#: 之前的数据）。固件默认关闭，串口 `capture on` 打开。
TYPE_HOST_RAW = 0x12
TYPE_FEEDBACK = 0x20
#: 设备 → PC：实机截图（声明 / 分块 / 收尾），与固件 input_frame.h 同名。
TYPE_IMAGE_INFO = 0x21
TYPE_IMAGE_DATA = 0x22
TYPE_IMAGE_END = 0x23
TYPE_OTA_BEGIN = 0x30
TYPE_OTA_DATA = 0x31
TYPE_OTA_END = 0x32
TYPE_OTA_ACK = 0x33
#: amiibo 上传帧（设备 ← PC 的 BEGIN/DATA/END 与设备 → PC 的 ACK），载荷布局
#: 与固件 amiibo/amiibo_proto.h 一致：镜像固定 540 字节，逐帧回 ACK。
TYPE_AMIIBO_BEGIN = 0x40
TYPE_AMIIBO_DATA = 0x41
TYPE_AMIIBO_END = 0x42
TYPE_AMIIBO_ACK = 0x43
TYPE_PING = 0x7F

#: BEGIN 载荷：magic + image_size(u32 LE)。
OTA_BEGIN_MAGIC = b"ROM1"
#: DATA 载荷：seq(u16 LE) + 数据，单帧数据上限 200 字节。
OTA_DATA_MAX = 200
#: ACK 载荷：state + code + next_seq(u16 LE) + received(u32 LE)。
OTA_ACK_LEN = 8
#: BEGIN 的 ACK 在末尾追加的运行版本字段长度（ASCII，NUL 填充）。
OTA_ACK_VERSION_LEN = 16
#: 一个窗口的帧数：设备每收满这么多帧回一次 ACK，PC 收到才发下一窗。
OTA_WINDOW_FRAMES = 16
#: 数据帧的 slot 字段取这个值表示「这一帧是窗口的最后一帧」（含末尾不足一窗），
#: 设备收到即回应答，否则末尾那批帧要等固定帧数或超时。
OTA_SLOT_WINDOW_END = 1

OTA_STATE_NAMES = {0: "idle", 1: "receiving", 2: "done", 3: "failed"}
OTA_CODE_NAMES = {
    0: "ok",
    1: "设备忙（已有升级在进行或镜像待验证）",
    2: "镜像头无效",
    3: "序号不连续",
    4: "写 flash 失败",
    5: "字节数与声明不符",
    6: "镜像校验失败",
    7: "设备侧超时",
}

#: BEGIN 载荷：name_len(u8) + name(UTF-8) + 镜像大小(u32 LE)。
AMIIBO_NAME_MAX = 31
#: DATA 载荷：offset(u16 LE) + 数据，单帧数据上限 200 字节。
AMIIBO_DATA_MAX = 200
#: NTAG215 用户区完整镜像（amiibo dump 通行尺寸）。
AMIIBO_TAG_SIZE = 540
#: 厂商签名（READ_SIG 页）长度；572 字节 dump 把它附在镜像尾部。
AMIIBO_SIG_SIZE = 32
#: 带签名的整份 dump（镜像 + 签名）。
AMIIBO_FULL_SIZE = AMIIBO_TAG_SIZE + AMIIBO_SIG_SIZE
#: ACK 载荷：state + code + received(u32 LE) + slot（仅 done 有意义，0xFF 无）。
AMIIBO_ACK_LEN = 7

AMIIBO_STATE_NAMES = {0: "idle", 1: "receiving", 2: "done", 3: "failed"}
#: 数值与固件 amiibo_code_t 一致（测试与调用方按名字取用）。
AMIIBO_CODE_OK = 0
AMIIBO_CODE_BUSY = 1
AMIIBO_CODE_BAD_HEADER = 2
AMIIBO_CODE_OFFSET_ERROR = 3
AMIIBO_CODE_STORE_ERROR = 4
AMIIBO_CODE_SIZE_MISMATCH = 5
AMIIBO_CODE_TIMEOUT = 7
AMIIBO_CODE_NAMES = {
    0: "ok",
    1: "设备忙（已有上传在进行）",
    2: "名称或镜像大小无效",
    3: "数据偏移不衔接",
    4: "设备存储失败（槽位写满或 NVS 出错）",
    5: "字节数与声明不符",
    7: "设备侧超时",
}


def amiibo_begin_payload(name: str, size: int) -> bytes:
    """BEGIN 载荷：名称长度 + 名称（UTF-8，1-31 字节）+ 镜像字节数（小端）。"""
    raw = name.encode("utf-8")
    if not 0 < len(raw) <= AMIIBO_NAME_MAX:
        raise ValueError(f"amiibo 名称必须是 1-{AMIIBO_NAME_MAX} 字节（UTF-8），当前 {len(raw)}")
    return bytes([len(raw)]) + raw + size.to_bytes(4, "little")


def amiibo_data_payload(offset: int, chunk: bytes) -> bytes:
    """DATA 载荷：偏移（小端）+ 镜像数据。"""
    if len(chunk) > AMIIBO_DATA_MAX:
        raise ValueError("amiibo 数据块超过单帧上限")
    return offset.to_bytes(2, "little") + chunk


def parse_amiibo_ack(payload: bytes) -> dict:
    """解析设备回发的上传 ACK：state + code + received(u32 LE) + slot。"""
    if len(payload) < AMIIBO_ACK_LEN:
        raise ValueError(f"ACK 载荷过短：{len(payload)} 字节")
    return {
        "state": AMIIBO_STATE_NAMES.get(payload[0], str(payload[0])),
        "state_id": payload[0],
        "code": AMIIBO_CODE_NAMES.get(payload[1], str(payload[1])),
        "code_id": payload[1],
        "received": int.from_bytes(payload[2:6], "little"),
        "slot": payload[6],
    }

FAMILY_UNKNOWN = 0
FAMILY_XBOX = 1
FAMILY_PS = 2
FAMILY_STEAM = 3

CONN_UNKNOWN = 0
CONN_USB = 1
CONN_BT = 2

#: 已知厂商 → 家族。固件侧还会按同样的 VID 再判一次，这里只是随帧带过去的提示。
VENDOR_FAMILY = {
    0x045E: FAMILY_XBOX,
    0x054C: FAMILY_PS,
    0x28DE: FAMILY_STEAM,
}

FAMILY_NAMES = {
    FAMILY_UNKNOWN: "unknown",
    FAMILY_XBOX: "xbox",
    FAMILY_PS: "ps",
    FAMILY_STEAM: "steam",
}


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE（多项式 0x1021，初值 0xFFFF）。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode(frame_type: int, slot: int, seq: int, payload: bytes = b"",
           max_payload: int = MAX_PAYLOAD) -> bytes:
    if len(payload) > max_payload:
        raise ValueError("载荷超过该帧类型允许的上限")
    frame = bytes([SYNC0, SYNC1, VERSION, frame_type, slot, seq, len(payload)]) + payload
    crc = crc16(frame)
    return frame + bytes([crc & 0xFF, crc >> 8])


def ota_begin_payload(image_size: int) -> bytes:
    """BEGIN 载荷：magic + 镜像字节数（小端）。"""
    return OTA_BEGIN_MAGIC + image_size.to_bytes(4, "little")


def ota_data_payload(seq: int, chunk: bytes) -> bytes:
    """DATA 载荷：块序号（小端）+ 镜像数据。"""
    if len(chunk) > OTA_DATA_MAX:
        raise ValueError("OTA 数据块超过单帧上限")
    return seq.to_bytes(2, "little") + chunk


def parse_ota_ack(payload: bytes) -> dict:
    """解析设备回发的 ACK；BEGIN 的应答末尾带 16 字节运行版本。"""
    if len(payload) < OTA_ACK_LEN:
        raise ValueError(f"ACK 载荷过短：{len(payload)} 字节")
    version = b""
    if len(payload) >= OTA_ACK_LEN + OTA_ACK_VERSION_LEN:
        version = payload[OTA_ACK_LEN : OTA_ACK_LEN + OTA_ACK_VERSION_LEN].split(b"\0")[0]
    return {
        "state": OTA_STATE_NAMES.get(payload[0], str(payload[0])),
        "state_id": payload[0],
        "code": OTA_CODE_NAMES.get(payload[1], str(payload[1])),
        "code_id": payload[1],
        "next_seq": int.from_bytes(payload[2:4], "little"),
        "received": int.from_bytes(payload[4:8], "little"),
        "version": version.decode("utf-8", errors="replace"),
    }


def parse_image_info(payload: bytes) -> tuple[int, int, int]:
    """解析截图声明：返回（宽, 高, 像素格式）。"""
    if len(payload) < IMAGE_INFO_LEN:
        raise ValueError(f"截图声明过短：{len(payload)} 字节")
    width = int.from_bytes(payload[0:2], "little")
    height = int.from_bytes(payload[2:4], "little")
    return width, height, payload[4]


def parse_image_chunk(payload: bytes) -> tuple[int, bytes]:
    """解析截图分块：返回（整幅画面的字节偏移, 像素数据）。"""
    if len(payload) < IMAGE_OFF_LEN:
        raise ValueError(f"截图分块过短：{len(payload)} 字节")
    return int.from_bytes(payload[0:IMAGE_OFF_LEN], "little"), payload[IMAGE_OFF_LEN:]


def parse_image_end(payload: bytes) -> int:
    """解析截图收尾：返回整幅画面的总字节数。"""
    if len(payload) < 4:
        raise ValueError(f"截图收尾过短：{len(payload)} 字节")
    return int.from_bytes(payload[0:4], "little")


def device_id(family: int, conn: int, vid: int, pid: int, report_id: int, report_len: int) -> bytes:
    """设备标识载荷：家族、连接方式、VID/PID 小端、Report ID 与报告长度。"""
    return bytes([
        family,
        conn,
        vid & 0xFF,
        (vid >> 8) & 0xFF,
        pid & 0xFF,
        (pid >> 8) & 0xFF,
        report_id,
        report_len,
    ])


def family_for_vendor(vid: int) -> int:
    return VENDOR_FAMILY.get(vid, FAMILY_UNKNOWN)


#: FEEDBACK 载荷里两带频率落地值（字节 8-15，小端 u16 ×4）的偏移；
#: 12 字节 = 老固件（无频率字段），PC 按长度判断。
FEEDBACK_FREQ_OFF = 8
FEEDBACK_LEN = 16
#: 57 字节版本再带 HD 时序子帧表（固件按布局行 hd 规则重整出的子帧序列，
#: PC 侧音频触觉与蓝牙私有流按它哑渲染）：[16] 左子帧数 + 3×（低频频率 u16 LE
#: + 低频增益 u8 + 高频频率 u16 LE + 高频增益 u8），[35] 右子帧数 + 3 子帧，
#: [54:56] 扬声器频率、[56] 扬声器增益。
FEEDBACK_HD_LEN = 57
FEEDBACK_HD_KEY_MAX = 3


def _feedback_keys(payload: bytes, base: int) -> dict:
    count = payload[base]
    keys = []
    for k in range(FEEDBACK_HD_KEY_MAX):
        off = base + 1 + k * 6
        lf = payload[off] | payload[off + 1] << 8
        lg = payload[off + 2]
        hf = payload[off + 3] | payload[off + 4] << 8
        hg = payload[off + 5]
        if k < count:
            keys.append(((lf, lg), (hf, hg)))
    return {"count": count, "keys": tuple(keys)}


def feedback_params(payload: bytes) -> dict | None:
    """FEEDBACK 帧载荷 → 参数字典（音频触觉合成与打印共用）。

    与固件 pad_feedback_wire 的载荷布局一致：[0]/[1] 左右使能、[2]/[3] 低频
    强度、[4] 玩家灯、[5] 触觉采样（0 = 无）、[6]/[7] 高频强度；16 字节版本
    再带 [8:16] 两带驱动频率落地值（低频 L/R、高频 L/R，Hz）；57 字节版本
    再带 HD 时序子帧表（每侧子帧按时间顺序各播 1/3 周期 + 扬声器音色，映射
    已在固件布局内完成）。载荷过短返回 None。
    """
    if len(payload) < 8:
        return None
    params = {
        "rumble_on": (bool(payload[0]), bool(payload[1])),
        "lf_amp": (payload[2], payload[3]),
        "player_led": payload[4],
        "sample": payload[5],
        "hf_amp": (payload[6], payload[7]),
        "lf_freq": None,
        "hf_freq": None,
        "hd": None,
    }
    if len(payload) >= FEEDBACK_LEN:
        lf_l, lf_r, hf_l, hf_r = struct.unpack_from("<4H", payload, FEEDBACK_FREQ_OFF)
        params["lf_freq"] = (lf_l, lf_r)
        params["hf_freq"] = (hf_l, hf_r)
    if len(payload) >= FEEDBACK_HD_LEN:
        params["hd"] = {
            "l": _feedback_keys(payload, 16),
            "r": _feedback_keys(payload, 35),
            "speaker": (payload[54] | payload[55] << 8, payload[56]),
        }
    return params


#: HOST_RAW 载荷头：通道字节 + 标志/长度字节（bit7 = 截断，低 7 位 = 数据长度）。
HOST_RAW_HEADER = 2
#: 通道字节 → 名字（GATT 属性表的句柄低字节，与固件
#: dp_capture.h 同一张表）。未登记的通道按 ch-<hex> 显示。
HOST_RAW_CHANNELS = {
    0x05: "base-config",
    0x12: "rumble",
    0x14: "cmd",
    0x16: "composite",
    0x18: "fwupg",
    0x22: "ext-22",
    0x26: "ext-26",
    0x2A: "ext-2a",
    0x2C: "ext-2c",
    0x2E: "ext-2e",
    0x32: "ext-32",
}


def parse_host_raw(payload: bytes) -> dict:
    """采集帧载荷 → {channel, name, truncated, data}。

    与固件 dp_capture.c 的载荷布局一致：[0] 通道、[1] 标志/长度、[2:] 原始
    字节（主机写进输出特征值的原始数据，布局解析之前）。
    """
    if len(payload) < HOST_RAW_HEADER:
        raise ValueError(f"采集帧载荷过短：{len(payload)} 字节")
    channel = payload[0]
    flags = payload[1]
    length = flags & 0x7F
    data = payload[HOST_RAW_HEADER:HOST_RAW_HEADER + length]
    return {
        "channel": channel,
        "name": HOST_RAW_CHANNELS.get(channel, f"ch-{channel:02x}"),
        "truncated": bool(flags & 0x80),
        "data": data,
    }


class FrameDecoder:
    """与固件 input_frame_rx 同一套重新对齐规则：帧交给调用方，文本丢弃或回调。"""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> tuple[list[tuple[int, int, int, bytes]], bytes]:
        """喂入字节，返回（帧列表, 非帧文本）。帧元素为 (type, slot, seq, payload)。"""
        self._buf.extend(data)
        frames: list[tuple[int, int, int, bytes]] = []
        text = bytearray()
        while True:
            start = self._buf.find(bytes([SYNC0, SYNC1]))
            if start < 0:
                # 末字节可能是同步字前半，留到下一批。
                keep = 1 if self._buf[-1:] == bytes([SYNC0]) else 0
                if len(self._buf) > keep:
                    text.extend(self._buf[: len(self._buf) - keep])
                    del self._buf[: len(self._buf) - keep]
                break
            if start > 0:
                text.extend(self._buf[:start])
                del self._buf[:start]
            if len(self._buf) < HEADER_LEN:
                break
            payload_len = self._buf[6]
            if payload_len > DECODE_MAX_PAYLOAD:
                del self._buf[:1]
                continue
            total = HEADER_LEN + payload_len + CRC_LEN
            if len(self._buf) < total:
                break
            want = self._buf[total - 2] | (self._buf[total - 1] << 8)
            if crc16(bytes(self._buf[: total - CRC_LEN])) != want:
                del self._buf[:1]
                continue
            frames.append((
                self._buf[3],
                self._buf[4],
                self._buf[5],
                bytes(self._buf[HEADER_LEN : HEADER_LEN + payload_len]),
            ))
            del self._buf[:total]
        return frames, bytes(text)


_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_OPEN_EXISTING = 3
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_PURGE_RXCLEAR = 0x0008
_DTR_CONTROL_DISABLE = 0x00
_RTS_CONTROL_DISABLE = 0x00
_RTS_CONTROL_ENABLE = 0x01
_MAXDWORD = 0xFFFFFFFF

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateFileW.restype = wintypes.HANDLE
_kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_void_p,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]


class _Dcb(ctypes.Structure):
    """Win32 DCB：DTR/RTS 的状态就在这里，打开前先定成禁用。"""

    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("fBinary", wintypes.DWORD, 1),
        ("fParity", wintypes.DWORD, 1),
        ("fOutxCtsFlow", wintypes.DWORD, 1),
        ("fOutxDsrFlow", wintypes.DWORD, 1),
        ("fDtrControl", wintypes.DWORD, 2),
        ("fDsrSensitivity", wintypes.DWORD, 1),
        ("fTXContinueOnXoff", wintypes.DWORD, 1),
        ("fOutX", wintypes.DWORD, 1),
        ("fInX", wintypes.DWORD, 1),
        ("fErrorChar", wintypes.DWORD, 1),
        ("fNull", wintypes.DWORD, 1),
        ("fRtsControl", wintypes.DWORD, 2),
        ("fAbortOnError", wintypes.DWORD, 1),
        ("fDummy2", wintypes.DWORD, 17),
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", wintypes.BYTE),
        ("Parity", wintypes.BYTE),
        ("StopBits", wintypes.BYTE),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class _CommTimeouts(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


# 句柄参数必须声明成 HANDLE：ctypes 默认会把 Python int 当 32 位参数传。
_kernel32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Dcb)]
_kernel32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Dcb)]
_kernel32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(_CommTimeouts)]
_kernel32.SetupComm.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
_kernel32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_kernel32.ReadFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_kernel32.WriteFile.argtypes = list(_kernel32.ReadFile.argtypes)
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def _fail(what: str) -> OSError:
    code = ctypes.get_last_error()
    return OSError(code, f"{what}失败（Win32 错误 {code}）")


class SerialLink:
    """USB-Serial/JTAG 串口：打开与运行期间 DTR、RTS 始终为低。"""

    def __init__(self, port: str, baud: int = 115200, read_timeout_ms: int = 0) -> None:
        self._rx = b""
        self._handle = _kernel32.CreateFileW(
            "\\\\.\\" + port, _GENERIC_READ | _GENERIC_WRITE, 0, None, _OPEN_EXISTING, 0, None
        )
        if self._handle == _INVALID_HANDLE_VALUE:
            raise _fail(f"打开 {port}")
        dcb = _Dcb()
        if not _kernel32.GetCommState(self._handle, ctypes.byref(dcb)):
            self.close()
            raise _fail("读取串口配置")
        dcb.BaudRate = baud
        dcb.fBinary = 1
        dcb.fParity = 0
        dcb.fOutxCtsFlow = 0
        dcb.fOutxDsrFlow = 0
        dcb.fDtrControl = _DTR_CONTROL_DISABLE
        dcb.fDsrSensitivity = 0
        dcb.fTXContinueOnXoff = 0
        dcb.fOutX = 0
        dcb.fInX = 0
        dcb.fErrorChar = 0
        dcb.fNull = 0
        dcb.fRtsControl = _RTS_CONTROL_DISABLE
        dcb.fAbortOnError = 0
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        if not _kernel32.SetCommState(self._handle, ctypes.byref(dcb)):
            self.close()
            raise _fail("应用串口配置")
        timeouts = _CommTimeouts(_MAXDWORD, 0, read_timeout_ms, 0, 1000)
        if not _kernel32.SetCommTimeouts(self._handle, ctypes.byref(timeouts)):
            self.close()
            raise _fail("设置串口超时")
        _kernel32.SetupComm(self._handle, 4096, 4096)
        _kernel32.PurgeComm(self._handle, _PURGE_RXCLEAR)

    def read(self, size: int = 4096) -> bytes:
        buf = ctypes.create_string_buffer(size)
        got = wintypes.DWORD(0)
        if not _kernel32.ReadFile(self._handle, buf, size, ctypes.byref(got), None):
            raise _fail("读取串口")
        return buf.raw[: got.value]

    def write(self, data: bytes) -> None:
        buf = ctypes.create_string_buffer(data, len(data))
        sent = 0
        while sent < len(data):
            written = wintypes.DWORD(0)
            if not _kernel32.WriteFile(
                self._handle, ctypes.byref(buf, sent), len(data) - sent, ctypes.byref(written), None
            ):
                raise _fail("写入串口")
            if written.value == 0:
                raise OSError("写入串口失败：未接受任何字节")
            sent += written.value

    def readline(self) -> bytes:
        """返回一行（含换行）；没有完整行时返回手上的残行，完全没数据返回空。"""
        while True:
            end = self._rx.find(b"\n")
            if end >= 0:
                line, self._rx = self._rx[: end + 1], self._rx[end + 1 :]
                return line
            chunk = self.read()
            if not chunk:
                line, self._rx = self._rx, b""
                return line
            self._rx += chunk

    def purge_input(self) -> None:
        """丢掉接收缓冲里还没读的数据（命令前后对齐用）。"""
        self._rx = b""
        _kernel32.PurgeComm(self._handle, _PURGE_RXCLEAR)

    def flush(self) -> None:
        """等待发送缓冲里的字节真正写出去。"""
        _kernel32.FlushFileBuffers(self._handle)

    def pulse_reset(self) -> None:
        """硬复位：DTR 保持低，RTS 拉高 120 ms 再放下（esptool 的复位脉冲）。"""
        self._set_rts(True)
        time.sleep(0.12)
        self._set_rts(False)
        self.purge_input()

    def _set_rts(self, level: bool) -> None:
        dcb = _Dcb()
        if not _kernel32.GetCommState(self._handle, ctypes.byref(dcb)):
            self.close()
            raise _fail("读取串口配置")
        dcb.fDtrControl = _DTR_CONTROL_DISABLE
        dcb.fRtsControl = _RTS_CONTROL_ENABLE if level else _RTS_CONTROL_DISABLE
        if not _kernel32.SetCommState(self._handle, ctypes.byref(dcb)):
            raise _fail("设置 RTS")
    def close(self) -> None:
        if self._handle:
            _kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "SerialLink":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def open_port(port: str, baud: int = 115200) -> SerialLink:
    """打开端口，失败时给出常见原因的提示并以环境问题（退出码 2）结束。"""
    try:
        return SerialLink(port, baud)
    except OSError as exc:
        print(f"{port}: {open_hint(exc)}", file=sys.stderr)
        raise SystemExit(2)


def open_hint(exc: OSError) -> str:
    """把打开端口失败映射成一句可读原因（图形界面直接显示这句）。"""
    code = exc.errno or 0
    if code in (5, 32):
        return "端口被占用，先结束占用进程（idf.py monitor、桥接程序等）"
    if code == 2:
        return "端口不存在，确认设备已插好（Get-PnpDevice -Class Ports）"
    return "打开端口失败"


def port_sort_key(name: str) -> tuple[str, int]:
    """COM 口按编号排序；不叫 COM<n> 的名字排在后面并按名字序。"""
    digits = name[3:]
    return ("", int(digits)) if name.upper().startswith("COM") and digits.isdigit() else (name, 0)


def serial_port_names(values: list[tuple[str, str]]) -> list[str]:
    """注册表里的 (设备名, 端口名) 列表 → 去重排序后的端口名列表。"""
    return sorted({port for _device, port in values if port.upper().startswith("COM")},
                  key=port_sort_key)


def list_serial_ports() -> list[str]:
    """枚举本机串口：读注册表的 SERIALCOMM 键，读不到就返回空表。

    命令行与图形界面都只用这份列表挑口（USB-Serial/JTAG、蓝牙调制解调器、
    虚拟串口都会出现，用哪一个是用户的选择）。
    """
    import winreg  # 仅在 Windows 上存在，延迟导入保证非 Windows 先走上面的平台检查。

    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
            values: list[tuple[str, str]] = []
            index = 0
            while True:
                try:
                    device, port, _kind = winreg.EnumValue(key, index)
                except OSError:
                    break
                values.append((device, port))
                index += 1
    except OSError:
        return []
    return serial_port_names(values)

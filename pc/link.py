#!/usr/bin/env python3
"""Remapad 桥接链路的 PC 侧：免复位串口打开 + 桥接帧编解码。

串口打开沿用 scripts/uartctl.py 的做法：直接用 Win32 API，并在打开前后把
DTR/RTS 固定为低电平——USB-Serial/JTAG 的片内状态机把这两条线当复位控制线
解释（RTS 拉高即复位），普通串口库默认会在打开端口时拉起它们。

帧格式与固件侧 input_frame.c 一致：
    A5 5A | ver | type | slot | seq | len | payload[len] | crc16(LE)
CRC-16/CCITT-FALSE 覆盖除末尾两字节外的整帧（含同步字）。
"""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（与 scripts/uartctl.py 同一做法）。
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
MAX_PAYLOAD = 72
MAX_FRAME = HEADER_LEN + MAX_PAYLOAD + CRC_LEN

TYPE_ATTACH = 0x01
TYPE_DETACH = 0x02
TYPE_REPORT = 0x10
TYPE_FEEDBACK = 0x20
TYPE_PING = 0x7F

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


def encode(frame_type: int, slot: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("载荷超过桥接帧上限")
    frame = bytes([SYNC0, SYNC1, VERSION, frame_type, slot, seq, len(payload)]) + payload
    crc = crc16(frame)
    return frame + bytes([crc & 0xFF, crc >> 8])


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
            if payload_len > MAX_PAYLOAD:
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

    def close(self) -> None:
        if self._handle:
            _kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "SerialLink":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

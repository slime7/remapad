#!/usr/bin/env python3
"""Remapad 串口控制台客户端。

通过 USB-Serial/JTAG COM 口发送行命令（固件侧 remapad-cli），用于验收时
免去手点屏幕。与 idf.py monitor 共用端口，二者不要同时打开。

用法：
    python scripts/uartctl.py -p COM3 status
    python scripts/uartctl.py -p COM3 key a
    python scripts/uartctl.py -p COM3 backlight 60
    python scripts/uartctl.py -p COM3 screen off
    python scripts/uartctl.py -p COM3 pairing start
    python scripts/uartctl.py -p COM3 log --seconds 20
    python scripts/uartctl.py -p COM3 log --seconds 25 --reset

不带命令时进入交互模式（q 退出）。

实现说明：只用 Python 标准库（ctypes 直调 Win32 串口 API），不需要安装第三方包。
原因在 ESP32-S3 的 USB-Serial/JTAG（USJ）侧：片内状态机把 CDC 的 DTR/RTS 当复位
控制线解释，RTS 拉高就是一次芯片复位，DTR 与 RTS 同时拉高会让芯片停止运行应用
（只能靠复位脉冲恢复），
而多数串口库默认在打开端口时拉起这两条线，于是表现为「连上就复位」。本工具在
SetCommState 里把两条控制线显式置为禁用，打开前后都保持低电平，因此连接设备不会
打断它正在做的事；需要复位时才用 --reset（只把 RTS 脉冲一下，DTR 全程为低）。
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import time
from ctypes import wintypes

if sys.platform != "win32":
    print(
        "uartctl.py 目前只有 Windows 实现（直接使用 Win32 串口 API，以保证打开端口时"
        "不拉起 DTR/RTS）。其他平台需要另行实现，见 docs/GETTING-STARTED.md。",
        file=sys.stderr,
    )
    raise SystemExit(2)

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

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
_kernel32.ReadFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_kernel32.WriteFile.argtypes = list(_kernel32.ReadFile.argtypes)


class _Dcb(ctypes.Structure):
    """Win32 DCB：串口的设备控制块，DTR/RTS 的状态就在这里。"""

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
_kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def _fail(what: str) -> OSError:
    code = ctypes.get_last_error()
    return OSError(code, f"{what}失败（Win32 错误 {code}）")


class SerialPort:
    """USB-Serial/JTAG 串口：打开与运行期间 DTR、RTS 始终为低。

    DTR 保持低避免进入下载模式，RTS 保持低避免复位芯片；只有 pulse_reset()
    会短暂把 RTS 拉高。读语义：read() 在超时内返回已到达的数据，readline()
    返回一行，没有完整行时返回手上的残行，完全没数据时返回空。
    """

    def __init__(self, port: str, baud: int = 115200, read_timeout_s: float = 0.1):
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

        timeouts = _CommTimeouts(
            ReadIntervalTimeout=_MAXDWORD,
            ReadTotalTimeoutMultiplier=0,
            ReadTotalTimeoutConstant=int(read_timeout_s * 1000),
            WriteTotalTimeoutMultiplier=0,
            WriteTotalTimeoutConstant=1000,
        )
        if not _kernel32.SetCommTimeouts(self._handle, ctypes.byref(timeouts)):
            self.close()
            raise _fail("设置串口超时")
        _kernel32.SetupComm(self._handle, 4096, 4096)
        self.purge_input()

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
            remaining = len(data) - sent
            if not _kernel32.WriteFile(
                self._handle,
                ctypes.byref(buf, sent),
                remaining,
                ctypes.byref(written),
                None,
            ):
                raise _fail("写入串口")
            if written.value == 0:
                raise OSError("写入串口失败：未接受任何字节")
            sent += written.value

    def readline(self) -> bytes:
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
        self._rx = b""
        _kernel32.PurgeComm(self._handle, _PURGE_RXCLEAR)

    def flush(self) -> None:
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
            raise _fail("读取串口配置")
        dcb.fDtrControl = _DTR_CONTROL_DISABLE
        dcb.fRtsControl = _RTS_CONTROL_ENABLE if level else _RTS_CONTROL_DISABLE
        if not _kernel32.SetCommState(self._handle, ctypes.byref(dcb)):
            raise _fail("设置 RTS")

    def close(self) -> None:
        if self._handle:
            _kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "SerialPort":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def open_port(port: str, baud: int) -> SerialPort:
    try:
        return SerialPort(port, baud)
    except OSError as exc:
        code = exc.errno or 0
        if code in (5, 32):
            hint = "端口被占用，先结束占用进程（idf.py monitor、串口助手等）"
        elif code == 2:
            hint = "端口不存在，确认设备已插好（Get-PnpDevice -Class Ports）"
        else:
            hint = "打开端口失败"
        print(f"{port}: {hint}", file=sys.stderr)
        raise SystemExit(2)


def read_reply(ser: SerialPort, wait_s: float) -> list[str]:
    lines: list[str] = []
    deadline = time.time() + wait_s
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").strip()
        if text:
            lines.append(text)
            # 命令回复以 ok/err 开头，收到后尽快返回，避免等满超时。
            if text.startswith(("ok", "err", "pong")):
                deadline = min(deadline, time.time() + 0.15)
    return lines


def run_once(ser: SerialPort, command: str, wait_s: float) -> None:
    ser.purge_input()
    ser.write((command + "\r").encode("utf-8"))
    ser.flush()
    for line in read_reply(ser, wait_s):
        # 串口上会混有固件日志；命令回复原样透传即可区分。
        print(line)


def interactive(ser: SerialPort, wait_s: float) -> None:
    print("交互模式，q 退出；可用命令见固件 help。")
    while True:
        try:
            command = input("remapad> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if command in ("q", "quit", "exit"):
            break
        if command:
            run_once(ser, command, wait_s)


def stream_log(ser: SerialPort, seconds: float, reset: bool, raw: bool) -> None:
    if reset:
        ser.pulse_reset()
    started = time.time()
    try:
        while seconds <= 0 or time.time() - started < seconds:
            data = ser.readline()
            if not data:
                continue
            text = data.decode("utf-8", errors="replace").rstrip()
            if not text:
                continue
            if raw:
                print(text, flush=True)
            else:
                print(f"[{time.time() - started:7.2f}s] {text}", flush=True)
    except KeyboardInterrupt:
        pass


def run_log(ser: SerialPort, argv: list[str]) -> None:
    parser = argparse.ArgumentParser(prog="uartctl.py log", description="读取设备日志")
    parser.add_argument("--seconds", type=float, default=15.0,
                        help="读取时长（秒），0 表示持续到 Ctrl+C（默认 15）")
    parser.add_argument("--reset", action="store_true", help="先复位设备，从启动日志开始读")
    parser.add_argument("--raw", action="store_true", help="不加相对时间前缀")
    opts = parser.parse_args(argv)
    stream_log(ser, opts.seconds, opts.reset, opts.raw)


def main() -> None:
    parser = argparse.ArgumentParser(description="Remapad 串口控制台客户端")
    parser.add_argument("-p", "--port", default="COM3", help="串口名（默认 COM3）")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USJ 忽略）")
    parser.add_argument("--wait", type=float, default=1.2, help="回复等待秒数")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="行命令，如 status")
    args = parser.parse_args()

    with open_port(args.port, args.baud) as ser:
        time.sleep(0.3)
        if args.command and args.command[0] == "log":
            run_log(ser, args.command[1:])
        elif args.command:
            run_once(ser, " ".join(args.command), args.wait)
        else:
            interactive(ser, args.wait)


if __name__ == "__main__":
    main()

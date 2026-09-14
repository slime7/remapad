#!/usr/bin/env python3
"""Remapad 串口控制台客户端。

通过 USB-Serial/JTAG COM 口发送行命令（固件侧 remapad-cli），用于验收时免去
手点屏幕。与 idf.py monitor 共用端口，二者不要同时打开。

用法（在 pc/ 目录执行）：
    uv run python uartctl.py -p COM3 status
    uv run python uartctl.py -p COM3 key a
    uv run python uartctl.py -p COM3 version
    uv run python uartctl.py -p COM3 rollback
    uv run python uartctl.py -p COM3 log --seconds 20
    uv run python uartctl.py -p COM3 log --seconds 25 --reset

不带命令时进入交互模式（q 退出）。

串口打开、读取与复位脉冲都在 [link.py](link.py) 的 SerialLink 里：用 Win32 API
打开，且打开前后把 DTR/RTS 固定为低电平。原因是 USB-Serial/JTAG 的片内状态机
把这两条线当复位控制线解释（RTS 拉高即复位，两条同时拉高会让设备停在不运行
应用的状态），而多数串口库默认会把它们拉起来，表现为「连上就复位」。需要复位
时用 `log --reset`，它只脉冲 RTS。
"""

from __future__ import annotations

import argparse
import sys
import time

from link import SerialLink, open_port

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")


def read_reply(ser: SerialLink, wait_s: float) -> list[str]:
    """读命令回复：以 ok/err/pong 开头的行出现后尽快返回，避免等满超时。"""
    lines: list[str] = []
    deadline = time.time() + wait_s
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            continue
        lines.append(text)
        if text.startswith(("ok", "err", "pong")):
            deadline = min(deadline, time.time() + 0.15)
    return lines


def run_once(ser: SerialLink, command: str, wait_s: float) -> None:
    ser.purge_input()
    ser.write((command + "\r").encode("utf-8"))
    ser.flush()
    for line in read_reply(ser, wait_s):
        # 串口上会混有固件日志；命令回复原样透传即可区分。
        print(line)


def interactive(ser: SerialLink, wait_s: float) -> None:
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


def stream_log(ser: SerialLink, seconds: float, reset: bool, raw: bool) -> None:
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


def run_log(ser: SerialLink, argv: list[str]) -> None:
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


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

不带命令时进入交互模式（q 退出）。依赖 pyserial：pip install pyserial
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    print("缺少 pyserial：pip install pyserial", file=sys.stderr)
    sys.exit(2)


def read_reply(ser: serial.Serial, wait_s: float) -> list[str]:
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


def run_once(ser: serial.Serial, command: str, wait_s: float) -> None:
    ser.reset_input_buffer()
    ser.write((command + "\r").encode("utf-8"))
    ser.flush()
    for line in read_reply(ser, wait_s):
        # 串口上会混有固件日志；命令回复原样透传即可区分。
        print(line)


def interactive(ser: serial.Serial, wait_s: float) -> None:
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


def main() -> None:
    parser = argparse.ArgumentParser(description="Remapad 串口控制台客户端")
    parser.add_argument("-p", "--port", default="COM3", help="串口名（默认 COM3）")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USJ 忽略）")
    parser.add_argument("--wait", type=float, default=1.2, help="回复等待秒数")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="行命令，如 status")
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        time.sleep(0.3)
        if args.command:
            run_once(ser, " ".join(args.command), args.wait)
        else:
            interactive(ser, args.wait)


if __name__ == "__main__":
    main()

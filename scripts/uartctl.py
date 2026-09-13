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

不带命令时进入交互模式（q 退出）。依赖 pyserial：pip install pyserial

`log` 子命令只读设备日志（不改任何状态），每行前缀是本次读取的相对时间，
便于把「按下按键」「长按」这类人工动作和固件日志对上；加 --reset 会先复位
设备，用来抓包含启动画面的完整启动日志。
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


def reset_device(ser: serial.Serial) -> None:
    """USB-Serial/JTAG 硬复位：DTR 拉高 IO0（保持正常启动），RTS 脉冲复位。

    只用 RTS 脉冲（esptool 的 HardReset）：DTR 置位会把芯片带进下载模式，
    那样设备不启动、自然也读不到日志。
    """
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.reset_input_buffer()


def stream_log(ser: serial.Serial, seconds: float, reset: bool, raw: bool) -> None:
    if reset:
        reset_device(ser)
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


def run_log(ser: serial.Serial, argv: list[str]) -> None:
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

    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        time.sleep(0.3)
        if args.command and args.command[0] == "log":
            run_log(ser, args.command[1:])
        elif args.command:
            run_once(ser, " ".join(args.command), args.wait)
        else:
            interactive(ser, args.wait)


if __name__ == "__main__":
    main()

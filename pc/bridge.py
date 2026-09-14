#!/usr/bin/env python3
"""把 PC 手柄的原始 HID 报告转发给 Remapad（桥接链路）。

解析与映射都在固件侧：这里只做三件事——读手柄、按桥接帧格式发出去、把设备
回发的反馈帧打印出来。手柄报告布局的核对用 --dump。

用法（在 pc/ 目录下执行）：
    uv run python bridge.py --list                      # 列出候选手柄接口
    uv run python bridge.py --dump                      # 只打印原始报告（不接串口）
    uv run python bridge.py -p COM3                     # 自动挑第一只手柄转发
    uv run python bridge.py -p COM3 --vid 0x054C --pid 0x0CE6
    uv run python bridge.py -p COM3 --logs              # 同时打印设备日志

依赖 hidapi：在 pc/ 目录下 uv sync（或直接 uv run python bridge.py，会自动对齐环境）
"""

from __future__ import annotations

import argparse
import sys
import time

from link import (
    CONN_BT,
    CONN_UNKNOWN,
    CONN_USB,
    FAMILY_NAMES,
    TYPE_ATTACH,
    TYPE_DETACH,
    TYPE_FEEDBACK,
    TYPE_PING,
    TYPE_REPORT,
    FrameDecoder,
    SerialLink,
    device_id,
    encode,
    family_for_vendor,
)

try:
    import hid  # type: ignore
except ImportError:
    print("缺少 hidapi：在 pc/ 目录下执行 uv sync 后重试", file=sys.stderr)
    raise SystemExit(2)

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（与 scripts/uartctl.py 同一做法）。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

#: 手柄类接口：Generic Desktop / Joystick 与 Game Pad。
GAMEPAD_USAGE_PAGE = 0x01
GAMEPAD_USAGES = (0x04, 0x05)

CONN_NAMES = {CONN_UNKNOWN: "-", CONN_USB: "usb", CONN_BT: "bt"}


def list_candidates() -> list[dict]:
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


def pick_device(args) -> dict | None:
    for info in list_candidates():
        if args.vid is not None and info["vendor_id"] != args.vid:
            continue
        if args.pid is not None and info["product_id"] != args.pid:
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


def print_feedback(payload: bytes) -> None:
    if len(payload) < 6:
        return
    print(
        f"反馈 震动 L={'on' if payload[0] else 'off'} R={'on' if payload[1] else 'off'} "
        f"强度 {payload[2]}/{payload[3]} 玩家灯 0x{payload[4]:02x} 触觉 0x{payload[5]:02x}",
        flush=True,
    )


def run_dump(args) -> int:
    """只打印原始报告：用来核对固件家族表里的字段偏移。"""
    candidates = list_candidates()
    if not candidates:
        print("没有找到手柄接口（--list 可以看到全部候选）", file=sys.stderr)
        return 1
    info = candidates[0]
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


def run_bridge(args) -> int:
    with SerialLink(args.port) as link:
        decoder = FrameDecoder()
        seq = 0
        reports = 0
        frames = 0
        print(f"桥接已连接 {args.port}；等待手柄（--list 可查看候选）", flush=True)
        next_send = 0.0
        interval = 1.0 / args.max_rate if args.max_rate > 0 else 0.0
        last_stat = time.monotonic()
        device = None
        ident = b""
        try:
            while True:
                if device is None:
                    info = pick_device(args)
                    if info is None:
                        time.sleep(0.5)
                        continue
                    device = hid.device()
                    device.open_path(info["path"])
                    device.set_nonblocking(True)
                    ident = identity_payload(info, 0, 0)
                    link.write(encode(TYPE_ATTACH, 0, seq, ident))
                    print(f"设备接入：{describe(info)}", flush=True)

                data = device.read(64)
                now = time.monotonic()
                if data and now >= next_send:
                    raw = bytes(data)
                    payload = identity_payload(info, raw[0] if raw else 0, len(raw)) + raw
                    link.write(encode(TYPE_REPORT, 0, seq, payload))
                    seq = (seq + 1) & 0xFF
                    reports += 1
                    next_send = now + interval

                chunk = link.read()
                if chunk:
                    rx_frames, text = decoder.feed(chunk)
                    for frame_type, _slot, _seq, payload in rx_frames:
                        frames += 1
                        if frame_type == TYPE_FEEDBACK:
                            print_feedback(payload)
                        elif frame_type == TYPE_PING:
                            print(f"设备在线（协议 v{payload[0] if payload else 0}）", flush=True)
                    if args.logs and text:
                        sys.stdout.write(text.decode("utf-8", errors="replace"))
                        sys.stdout.flush()

                if not data and not chunk:
                    time.sleep(0.001)

                now = time.monotonic()
                if now - last_stat >= 5.0:
                    last_stat = now
                    print(f"已转发 {reports} 帧报告，收到设备帧 {frames} 个", flush=True)
        except KeyboardInterrupt:
            print("\n退出中…")
        except OSError as exc:
            print(f"链路错误：{exc}", file=sys.stderr)
            return 1
        finally:
            if device is not None:
                try:
                    link.write(encode(TYPE_DETACH, 0, seq, ident))
                except OSError:
                    pass
                device.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Remapad 手柄桥接（PC → 设备）")
    parser.add_argument("-p", "--port", default="COM3", help="串口名（默认 COM3）")
    parser.add_argument("--vid", type=lambda value: int(value, 0), help="只挑该厂商 ID")
    parser.add_argument("--pid", type=lambda value: int(value, 0), help="只挑该产品 ID")
    parser.add_argument("--list", action="store_true", help="列出候选手柄接口后退出")
    parser.add_argument("--dump", action="store_true", help="只打印原始报告，不接串口")
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="--dump 的采集时长（0 表示到 Ctrl+C）")
    parser.add_argument("--max-rate", type=float, default=250.0,
                        help="转发上限帧率（0 表示不限制，默认 250）")
    parser.add_argument("--logs", action="store_true", help="打印设备日志文本")
    args = parser.parse_args()

    if args.list:
        candidates = list_candidates()
        if not candidates:
            print("没有找到手柄接口")
            return 1
        for info in candidates:
            print(describe(info))
        return 0
    if args.dump:
        return run_dump(args)
    return run_bridge(args)


if __name__ == "__main__":
    raise SystemExit(main())

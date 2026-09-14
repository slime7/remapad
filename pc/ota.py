#!/usr/bin/env python3
"""Remapad 固件 OTA 上传（PC → 设备）。

把固件构建产物（默认 ../firmware/build/remapad_firmware.bin，已内嵌 .pocket）
经 USB-Serial/JTAG 推给设备：设备写进非运行应用分区，`esp_ota_end` 校验通过后
切换启动分区并重启。设备侧的协议、流控与回滚保护见 firmware/main/ota/。

用法（在 pc/ 目录执行）：
    uv run python ota.py -p COM3                     # 升级默认镜像
    uv run python ota.py -p COM3 --image <镜像路径>
    uv run python ota.py --dry-run --image <镜像路径>  # 只校验镜像，不接设备
    uv run python ota.py -p COM3 --wait              # 升级后等设备回来并打印版本
    uv run python ota.py -p COM3 --verbose           # 同时透传设备日志

升级期间设备会独占 COM 口：先退出 bridge.py、idf.py monitor 等占用进程。
设备仍在验证上一个镜像（开机 30 秒内的健康门槛）时会回 BUSY，等一会重试即可。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from link import (
    OTA_DATA_MAX,
    OTA_SLOT_WINDOW_END,
    OTA_WINDOW_FRAMES,
    TYPE_OTA_ACK,
    TYPE_OTA_BEGIN,
    TYPE_OTA_DATA,
    TYPE_OTA_END,
    FrameDecoder,
    SerialLink,
    encode,
    open_port,
    ota_begin_payload,
    ota_data_payload,
    parse_ota_ack,
    WIRE_MAX_PAYLOAD,
)

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码（与 uartctl.py 同一做法）。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

#: 应用镜像头 magic（ESP-IDF 二进制镜像格式）。
ESP_IMAGE_MAGIC = 0xE9
#: 镜像头里的芯片标识偏移与 ESP32-S3 取值。
ESP_CHIP_ID_OFFSET = 0x0C
ESP_CHIP_ID_ESP32S3 = 0x0009
#: 应用描述符偏移（24 字节镜像头 + 8 字节段头）与其 magic。
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + 0x10
APP_DESC_PROJECT_OFFSET = APP_DESC_OFFSET + 0x30
APP_DESC_FIELD_LEN = 32
#: 本工程的项目名：镜像里不是这个名字说明烧错了产物。
EXPECTED_PROJECT = "remapad_firmware"
#: 目标分区容量（partitions.csv 里 ota_0 / ota_1 各 4 MB）。
PARTITION_MAX_BYTES = 4 * 1024 * 1024

#: BEGIN 应答要等设备按声明大小预擦分区，给足时间；结束时等整体校验。
BEGIN_ACK_TIMEOUT_S = 20.0
ACK_TIMEOUT_S = 5.0
END_ACK_TIMEOUT_S = 30.0
#: 一个窗口超时后的重发次数（设备按序号去重，重复发送是安全的）。
MAX_WINDOW_RETRIES = 5
#: --wait 时重新打开端口的超时与轮询间隔。
REOPEN_TIMEOUT_S = 60.0
REOPEN_INTERVAL_S = 1.0

STATE_RECEIVING = 1
STATE_DONE = 2
STATE_FAILED = 3


def load_image(path: Path) -> tuple[bytes, str]:
    """读入并校验应用镜像，返回（字节, 版本号）；不合法直接报错退出。"""
    try:
        data = path.read_bytes()
    except OSError as exc:
        print(f"读不到镜像 {path}：{exc}", file=sys.stderr)
        raise SystemExit(2)
    if len(data) < APP_DESC_PROJECT_OFFSET + APP_DESC_FIELD_LEN:
        print(f"{path} 只有 {len(data)} 字节，不是应用镜像", file=sys.stderr)
        raise SystemExit(2)
    if data[0] != ESP_IMAGE_MAGIC:
        print(f"{path} 首字节是 0x{data[0]:02x}，不是 ESP-IDF 应用镜像", file=sys.stderr)
        raise SystemExit(2)
    chip = int.from_bytes(data[ESP_CHIP_ID_OFFSET : ESP_CHIP_ID_OFFSET + 2], "little")
    if chip != ESP_CHIP_ID_ESP32S3:
        print(f"{path} 的芯片标识是 0x{chip:04x}，不是 ESP32-S3", file=sys.stderr)
        raise SystemExit(2)
    magic = int.from_bytes(data[APP_DESC_OFFSET : APP_DESC_OFFSET + 4], "little")
    if magic != APP_DESC_MAGIC:
        print(f"{path} 缺少应用描述符（magic 0x{magic:08x}）", file=sys.stderr)
        raise SystemExit(2)
    version = _field(data, APP_DESC_VERSION_OFFSET)
    project = _field(data, APP_DESC_PROJECT_OFFSET)
    if project != EXPECTED_PROJECT:
        print(f"{path} 是 {project or '未知'} 的镜像，本设备只接受 {EXPECTED_PROJECT}",
              file=sys.stderr)
        raise SystemExit(2)
    if len(data) > PARTITION_MAX_BYTES:
        print(f"{path} 有 {len(data)} 字节，超过应用分区容量 {PARTITION_MAX_BYTES}",
              file=sys.stderr)
        raise SystemExit(2)
    return data, version


def _field(data: bytes, offset: int) -> str:
    raw = data[offset : offset + APP_DESC_FIELD_LEN].split(b"\0")[0]
    return raw.decode("utf-8", errors="replace")


class DeviceLink:
    """串口上的设备侧：发帧、按需等 ACK，顺带透传设备日志。"""

    def __init__(self, ser: SerialLink, verbose: bool) -> None:
        self._ser = ser
        self._verbose = verbose
        self._decoder = FrameDecoder()
        self._acks: list[dict] = []

    def send(self, data: bytes) -> None:
        self._ser.write(data)
        self._ser.flush()

    def reset(self) -> None:
        """清掉链路两侧的残留字节，从干净状态开始。"""
        self._ser.purge_input()
        self._decoder = FrameDecoder()
        self._acks.clear()

    def pump(self, timeout_s: float) -> dict | None:
        """等一个 OTA ACK；超时返回 None，期间把设备日志透传出来。"""
        deadline = time.monotonic() + timeout_s
        while True:
            if self._acks:
                return self._acks.pop(0)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            chunk = self._ser.read()
            if not chunk:
                time.sleep(0.001)
                continue
            frames, text = self._decoder.feed(chunk)
            for frame_type, _slot, _seq, payload in frames:
                if frame_type == TYPE_OTA_ACK:
                    self._acks.append(parse_ota_ack(payload))
            if text and self._verbose:
                sys.stdout.write(text.decode("utf-8", errors="replace"))
                sys.stdout.flush()

    def pump_final(self, timeout_s: float) -> dict | None:
        """等收尾应答：中间态 ACK（重发引起的）跳过，只认完成或失败。"""
        deadline = time.monotonic() + timeout_s
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ack = self.pump(remaining)
            if ack is None:
                return None
            if ack["state_id"] != STATE_RECEIVING:
                return ack


def describe_ack(ack: dict) -> str:
    version = f"，设备在跑 {ack['version']}" if ack.get("version") else ""
    return f"state={ack['state']} code={ack['code']}{version}"


def upload(ser: SerialLink, image: bytes, version: str, verbose: bool) -> int:
    """按窗口推送整幅镜像；返回进程退出码。"""
    link = DeviceLink(ser, verbose)
    link.reset()

    print(f"写入 {len(image)} 字节（镜像版本 {version}）")
    link.send(encode(TYPE_OTA_BEGIN, 0, 0, ota_begin_payload(len(image)),
                     max_payload=WIRE_MAX_PAYLOAD))
    ack = link.pump(BEGIN_ACK_TIMEOUT_S)
    if ack is None:
        print(f"设备没有在 {BEGIN_ACK_TIMEOUT_S:.0f} 秒内回应 BEGIN；"
              "确认 COM 口没被别的程序占用、设备不是 host 模式", file=sys.stderr)
        return 1
    if ack["state_id"] != STATE_RECEIVING or ack["code_id"] != 0:
        print(f"设备拒绝升级（{describe_ack(ack)}）；设备忙或镜像被拒时稍后重试",
              file=sys.stderr)
        return 1
    if ack["version"]:
        print(f"设备当前版本 {ack['version']} → 写入 {version}")

    confirmed = 0
    next_seq = 0
    retries = 0
    printed_pct = -1
    started = time.monotonic()
    while confirmed < len(image):
        offset = confirmed
        seq = next_seq
        frames: list[tuple[int, bytes]] = []
        while len(frames) < OTA_WINDOW_FRAMES and offset < len(image):
            chunk = image[offset : offset + OTA_DATA_MAX]
            frames.append((seq & 0xFFFF, chunk))
            offset += len(chunk)
            seq += 1
        window = bytearray()
        for index, (frame_seq, chunk) in enumerate(frames):
            # 窗口末帧带上标记：设备收到就回应答，末尾的不足一窗不必等超时。
            slot = OTA_SLOT_WINDOW_END if index == len(frames) - 1 else 0
            window += encode(TYPE_OTA_DATA, slot, frame_seq & 0xFF,
                             ota_data_payload(frame_seq, chunk),
                             max_payload=WIRE_MAX_PAYLOAD)
        link.send(bytes(window))
        ack = link.pump(ACK_TIMEOUT_S)
        if ack is None:
            retries += 1
            if retries > MAX_WINDOW_RETRIES:
                print(f"连续 {retries} 个窗口没有应答，升级中止；"
                      "设备侧 5 秒无数据会自行作废会话，仍从旧镜像启动", file=sys.stderr)
                return 1
            print(f"窗口应答超时，从 {confirmed} 字节处重发（第 {retries} 次）",
                      file=sys.stderr)
            continue
        retries = 0
        if ack["state_id"] == STATE_FAILED:
            print(f"设备中止升级（{describe_ack(ack)}），已收到 {ack['received']} 字节",
                  file=sys.stderr)
            return 1
        confirmed = ack["received"]
        next_seq = ack["next_seq"]
        pct = confirmed * 100 // len(image)
        if pct != printed_pct:
            printed_pct = pct
            print(f"  写入 {pct:3d}%（{confirmed}/{len(image)} 字节）", flush=True)

    elapsed = time.monotonic() - started
    print(f"数据传输完成，用时 {elapsed:.1f} 秒，等待设备校验镜像")
    link.send(encode(TYPE_OTA_END, 0, 0))
    ack = link.pump_final(END_ACK_TIMEOUT_S)
    if ack is None:
        print(f"设备没有在 {END_ACK_TIMEOUT_S:.0f} 秒内确认收尾", file=sys.stderr)
        return 1
    if ack["state_id"] != STATE_DONE or ack["code_id"] != 0:
        print(f"升级失败（{describe_ack(ack)}）；设备仍从旧镜像启动", file=sys.stderr)
        return 1
    print("升级完成：设备切到新分区并重启，首次启动会先处于「待验证」状态")
    return 0


def wait_for_version(port: str, baud: int) -> int:
    """等设备重启回来，问一次 version 命令并打印。"""
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
                    print(f"设备已回到 COM 口：{line}")
                    return 0
        finally:
            ser.close()
    print(f"{REOPEN_TIMEOUT_S:.0f} 秒内没有等到设备回到 {port}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Remapad 固件 OTA 上传")
    parser.add_argument("-p", "--port", default="COM3", help="串口名（默认 COM3）")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USJ 忽略）")
    parser.add_argument("--image", default="../firmware/build/remapad_firmware.bin",
                        help="应用镜像路径（默认 ../firmware/build/remapad_firmware.bin）")
    parser.add_argument("--dry-run", action="store_true", help="只校验镜像，不接设备")
    parser.add_argument("--wait", action="store_true",
                        help="升级后等设备重启回来并打印其版本")
    parser.add_argument("--verbose", action="store_true", help="透传设备日志文本")
    args = parser.parse_args()

    image_path = Path(args.image).expanduser()
    image, version = load_image(image_path)
    print(f"镜像 {image_path}：{len(image)} 字节，版本 {version}")
    if args.dry_run:
        print("dry-run：镜像校验通过，未连接设备")
        return 0

    with open_port(args.port, args.baud) as ser:
        code = upload(ser, image, version, args.verbose)
    if code != 0 or not args.wait:
        return code
    return wait_for_version(args.port, args.baud)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n已中断", file=sys.stderr)
        raise SystemExit(130)

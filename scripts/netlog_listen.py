"""局域网日志收听端：与固件 netlog 命令配套的 UDP 终端（临时调试工具）。

用法：uv run python scripts/netlog_listen.py [--port 9999] [--ip 设备IP]
设备侧 `netlog <ssid> <password>` 连上 AP 后（凭据可由 `netlog save` 预存、开机自连），
日志与 CLI 回复逐行打印，键入一行回车即作为 CLI 命令发往设备。
设备不知道 PC 的 IP：脚本会先向本机所在子网的广播地址发 hello（ping），
设备收到即学到目标地址；--ip 可指定点对点目标。Ctrl+C 退出。
"""

import argparse
import socket
import threading
import time

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "pc"))
import link  # noqa: E402  复用桥接帧的解码器，把 UDP 里的手柄帧与文本分开。


def local_broadcast_addrs() -> set[str]:
    """本机各 IPv4 网卡推导的 /24 广播地址（127.0.0.1 除外）。"""
    addrs: set[str] = set()
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        return addrs
    for info in infos:
        ip = info[4][0]
        if ip != "127.0.0.1" and ip.count(".") == 3:
            addrs.add(f"{ip.rsplit('.', 1)[0]}.255")
    return addrs


def main() -> None:
    parser = argparse.ArgumentParser(description="Remapad 局域网日志收听端（netlog 配套）")
    parser.add_argument("--port", type=int, default=link.NETLOG_PORT_DEFAULT,
                        help="UDP 端口，与固件 netlog 一致")
    parser.add_argument("--ip", default=None, help="设备 IP；不给则靠广播 hello 被设备学到")
    parser.add_argument("--no-hello", action="store_true", help="不发广播 hello（只收不发）")
    args = parser.parse_args()

    peer: list[str | None] = [args.ip]
    received = threading.Event()
    alive = threading.Event()
    alive.set()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(0.5)

    def receive() -> None:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except TimeoutError:
                continue
            except OSError:
                return
            if data == b"ping\n":
                continue  # 本机 hello 广播的回环，别把目标学成自己
            peer[0] = addr[0]
            received.set()
            stamp = time.strftime("%H:%M:%S")
            if data[:2] == b"\xa5\x5a":
                frames, _text = decoder.feed(data)
                for frame_type, slot, _seq, payload in frames:
                    print(f"[{stamp}] <帧 0x{frame_type:02x} slot={slot} {len(payload)}B", flush=True)
                continue
            text = data.decode("utf-8", "replace").rstrip("\r\n")
            if text:
                print(f"[{stamp}] {text}", flush=True)

    decoder = link.FrameDecoder()
    threading.Thread(target=receive, daemon=True).start()

    def hello_loop() -> None:
        """设备在等 PC 先说话才学得到目标：向子网广播（与 --ip）周期发 hello。"""
        targets = {(args.ip, args.port)} if args.ip else set()
        for bcast in local_broadcast_addrs():
            targets.add((bcast, args.port))
        if not targets:
            return
        while alive.is_set() and not received.is_set():
            for target in list(targets):
                try:
                    sock.sendto(b"ping\n", target)
                except OSError:
                    pass
            time.sleep(1.0)

    if not args.no_hello:
        threading.Thread(target=hello_loop, daemon=True).start()

    print(f"listening on udp/0.0.0.0:{args.port}，等设备 netlog 连上来"
          f"（{'广播' if not args.ip else args.ip} hello 已开启）…", flush=True)
    try:
        while True:
            line = input()
            if peer[0] is None:
                print("还没收到设备的数据包：确认设备已连上 WiFi（netlog 状态）", flush=True)
                continue
            sock.sendto((line + "\n").encode("utf-8"), (peer[0], args.port))
    except (KeyboardInterrupt, EOFError):
        print()
    finally:
        alive.clear()
        sock.close()


if __name__ == "__main__":
    main()

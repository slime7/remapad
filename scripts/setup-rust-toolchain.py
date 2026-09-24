#!/usr/bin/env python3
"""安装或检查屏幕 UI 需要的 Xtensa Rust 工具链。

用法：
    python scripts/setup-rust-toolchain.py                # 缺则装，缺什么装什么
    python scripts/setup-rust-toolchain.py --check        # 只检查，不改动环境

工具链已存在时只打印版本；否则下载 espup 的预编译二进制并执行 espup install。
离线或代理环境下可以自己装 espup（cargo install espup --locked）后重跑本脚本。
换机步骤、CMake 变量与日常命令见 ui/README.md。
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ESPUP_DOWNLOAD = "https://github.com/esp-rs/espup/releases/latest/download/espup-{asset}"

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

# 组件交叉编译用的目标三元组（固件界面组件只面向 ESP32-S3）。
TARGET_TRIPLE = "xtensa-esp32s3-none-elf"


def run(command: list[str], quiet: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(command, text=True, capture_output=quiet, check=False)


def toolchain_version(toolchain: str) -> str | None:
    """返回 cargo +<toolchain> 的版本行；工具链不存在时返回 None。"""
    result = run(["cargo", f"+{toolchain}", "--version"], quiet=True)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def rust_src_present(toolchain: str) -> bool:
    """rust-src 组件在位（-Zbuild-std=core,alloc 要用）。"""
    sysroot = run(["rustc", f"+{toolchain}", "--print", "sysroot"], quiet=True)
    if sysroot.returncode != 0:
        return False
    return Path(sysroot.stdout.strip(), "lib/rustlib/src/rust/library/core/Cargo.toml").is_file()


def target_supported(toolchain: str) -> bool:
    """工具链认识组件的目标三元组（防止名字指到普通工具链）。"""
    targets = run(["rustc", f"+{toolchain}", "--print", "target-list"], quiet=True)
    return targets.returncode == 0 and TARGET_TRIPLE in targets.stdout


def report(toolchain: str) -> int:
    """打印版本与 sysroot；缺 rust-src 或缺目标时给出修复命令并返回 1。"""
    version = toolchain_version(toolchain)
    if version is None:
        print(f"找不到 cargo +{toolchain}：检查工具链名与 rustup 环境", file=sys.stderr)
        return 1
    print(f"ok: {version}")
    sysroot = run(["rustc", f"+{toolchain}", "--print", "sysroot"], quiet=True)
    if sysroot.returncode == 0:
        print(f"sysroot: {sysroot.stdout.strip()}")
    if not rust_src_present(toolchain):
        hint = f"rustup component add rust-src --toolchain {toolchain}"
        print(f"缺 rust-src（-Zbuild-std 要用）：{hint}", file=sys.stderr)
        return 1
    if not target_supported(toolchain):
        hint = f"espup install -t esp32s3 -a {toolchain}"
        print(f"工具链不认识 {TARGET_TRIPLE}：确认它是 espup 装的 esp 工具链，或 {hint}", file=sys.stderr)
        return 1
    return 0


def host_asset() -> str:
    """espup 发布包里的宿主资源名。"""
    machine = platform.machine().lower()
    system = platform.system().lower()
    arm = machine in {"aarch64", "arm64"}
    if system == "windows":
        return "x86_64-pc-windows-msvc.zip"
    if system == "linux":
        return "aarch64-unknown-linux-gnu" if arm else "x86_64-unknown-linux-gnu"
    if system == "darwin":
        return "aarch64-apple-darwin" if arm else "x86_64-apple-darwin"
    raise SystemExit(f"不支持的平台：{system}/{machine}")


def download_espup(destination: Path) -> Path:
    """下载 espup 到 destination 并返回可执行文件路径。"""
    asset = host_asset()
    url = ESPUP_DOWNLOAD.format(asset=asset)
    print(f"download {url}")
    with urllib.request.urlopen(url) as response:
        payload = response.read()
    if asset.endswith(".zip"):
        archive = destination.with_suffix(".zip")
        archive.write_bytes(payload)
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(destination.parent)
        executable = destination.parent / "espup.exe"
    else:
        destination.write_bytes(payload)
        destination.chmod(0o755)
        executable = destination
    if not executable.is_file():
        raise SystemExit(f"espup 解包失败：{executable}")
    return executable


def main() -> int:
    parser = argparse.ArgumentParser(description="安装或检查 Xtensa Rust 工具链")
    parser.add_argument("--toolchain", default="esp", help="rustup 工具链名（默认 esp）")
    parser.add_argument("--targets", default="esp32s3", help="espup 目标列表（默认 esp32s3）")
    parser.add_argument("--check", action="store_true", help="只检查，不安装")
    args = parser.parse_args()

    if toolchain_version(args.toolchain) is not None:
        return report(args.toolchain)

    if args.check:
        print(f"缺少工具链 {args.toolchain}：espup install -t {args.targets}", file=sys.stderr)
        return 1
    if not all(shutil.which(tool) for tool in ("cargo", "rustc", "rustup")):
        print("未找到 rustup/cargo/rustc：先装 rustup（https://rustup.rs）", file=sys.stderr)
        return 1

    espup = shutil.which("espup")
    if espup is None:
        with tempfile.TemporaryDirectory(prefix="remapad-espup-") as workspace:
            espup = str(download_espup(Path(workspace) / "espup"))
            status = run([espup, "--version"])
            if status.returncode != 0:
                return 1
            code = run([espup, "install", "-t", args.targets, "-a", args.toolchain]).returncode
    else:
        code = run([espup, "install", "-t", args.targets, "-a", args.toolchain]).returncode
    if code != 0:
        print("espup install 失败；可改用 cargo install espup --locked 后重跑", file=sys.stderr)
        return code

    return report(args.toolchain)


if __name__ == "__main__":
    sys.exit(main())

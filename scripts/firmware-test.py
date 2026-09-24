#!/usr/bin/env python3
"""固件主机端单元测试入口：把与硬件无关的固件源码编译成开发机可执行文件并运行。

用法：
    python scripts/firmware-test.py

不走 ESP-IDF 的 Unity 上板跑：那套每次都要串口与实板，改一行代码也要等烧录；
这里要的是几秒钟内出结果的回归网，因此只挑不依赖 IDF 运行时的纯逻辑模块，
缺失的 IDF 头文件用 firmware/test/support/stubs 下的最小替身补齐，替身只参与本次编译。
编译器按 CC、MSVC（vcvars64）、clang、gcc 的顺序探测，任何一个都行。
用例范围与断言分层见 docs/TESTING.md。
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "firmware/build/host-tests"
OBJ_DIR = BUILD_DIR / "obj"
EXE_PATH = BUILD_DIR / ("remapad-host-tests.exe" if platform.system() == "Windows" else "remapad-host-tests")

"""被测固件源码：只列与硬件无关的模块。"""
FIRMWARE_SOURCES = [
    "firmware/main/pad/pad_state.c",
    "firmware/main/pad/layout.c",
    "firmware/main/pad/layouts/xbox.c",
    "firmware/main/pad/layouts/xinput.c",
    "firmware/main/pad/layouts/ds3.c",
    "firmware/main/pad/layouts/ds4.c",
    "firmware/main/pad/layouts/ds5.c",
    "firmware/main/pad/layouts/ns.c",
    "firmware/main/pad/pad_device.c",
    "firmware/main/pad/ds_behavior.c",
    "firmware/main/pad/feedback.c",
    "firmware/main/input/input_frame.c",
    "firmware/main/ota/ota_proto.c",
    "firmware/main/amiibo/amiibo_proto.c",
    "firmware/main/target/ns2/ns2_report.c",
    "firmware/main/target/ns2/ns2_adv.c",
    "firmware/main/target/ns2/ns2_serial.c",
    "firmware/main/target/ns2/ns2_frames.c",
    "firmware/main/target/ns2/ns2_nfc.c",
    "firmware/main/target/ns2/ns2_identity.c",
    "firmware/main/target/ns2/ns2_upgrade.c",
    "firmware/main/target/ns2/ns2_output.c",
    "firmware/main/target/target.c",
    "firmware/main/target/ns2/ns2_target.c",
    "firmware/main/dp/dp_source.c",
    "firmware/main/dp/dp_ui.c",
    "firmware/main/dp/dp_capture.c",
    "firmware/main/ui/ui_service.c",
    "firmware/main/drivers/battery_curve.c",
    "firmware/main/usb/usb_audio_parse.c",
    "firmware/main/usb/haptic_synth.c",
]

"""测试自身的源码与替身。"""
TEST_SOURCES = [
    "firmware/test/support/host_test.c",
    "firmware/test/suites.c",
    "firmware/test/support/stubs/app_config_stub.c",
    "firmware/test/support/stubs/heap_caps_stub.c",
    "firmware/test/support/stubs/esp_timer_stub.c",
    "firmware/test/support/stubs/ui_service_deps_stub.c",
    "firmware/test/test_ns2_report.c",
    "firmware/test/test_ns2_adv.c",
    "firmware/test/test_ns2_serial.c",
    "firmware/test/test_ns2_frames.c",
    "firmware/test/test_ns2_upgrade.c",
    "firmware/test/test_ns2_identity.c",
    "firmware/test/test_dp_source.c",
    "firmware/test/test_dp_ui.c",
    "firmware/test/test_dp_capture.c",
    "firmware/test/test_ui_service.c",
    "firmware/test/test_pad_device.c",
    "firmware/test/test_ds_behavior.c",
    "firmware/test/test_pad_ns.c",
    "firmware/test/test_pad_feedback.c",
    "firmware/test/test_ns2_relay.c",
    "firmware/test/test_input_frame.c",
    "firmware/test/test_ota_proto.c",
    "firmware/test/test_ns2_nfc.c",
    "firmware/test/test_amiibo_proto.c",
    "firmware/test/test_target_ns2.c",
    "firmware/test/test_battery.c",
    "firmware/test/test_usb_audio.c",
    "firmware/test/test_haptic_synth.c",
]

INCLUDE_DIRS = [
    "firmware/main",
    "firmware/main/pad",
    "firmware/main/input",
    "firmware/main/ota",
    "firmware/main/amiibo",
    "firmware/main/target",
    "firmware/main/target/ns2",
    "firmware/main/config",
    "firmware/main/dp",
    "firmware/main/drivers",
    "firmware/main/ui",
    "firmware/main/ble",
    "firmware/main/console",
    "firmware/main/usb",
    "firmware/test/support",
    "firmware/test/support/stubs",
]

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")


def _find_vcvars() -> Path | None:
    """定位 Visual Studio 的 vcvars64.bat：先用 vswhere，再扫常见安装目录。"""
    vswhere = Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    if vswhere.exists():
        probe = subprocess.run(
            [str(vswhere), "-products", "*", "-property", "installationPath"],
            text=True,
            capture_output=True,
            check=False,
        )
        for line in probe.stdout.splitlines():
            root = line.strip()
            if root == "":
                continue
            candidate = Path(root) / "VC/Auxiliary/Build/vcvars64.bat"
            if candidate.exists():
                return candidate
    bases = (Path("C:/Program Files/Microsoft Visual Studio"), Path("C:/Program Files (x86)/Microsoft Visual Studio"))
    for base in bases:
        if not base.is_dir():
            continue
        for version in base.iterdir():
            if not version.is_dir():
                continue
            for edition in version.iterdir():
                candidate = edition / "VC/Auxiliary/Build/vcvars64.bat"
                if candidate.exists():
                    return candidate
    return None


def _build_with_msvc(vcvars: Path) -> bool:
    """用 MSVC 编译：vcvars64 先配环境，再用响应文件传参数（省去 cmd 的引号问题）。"""
    # 路径统一写成正斜杠：MSVC 两种都收，而正斜杠不会和参数收尾引号打架
    # （/Fo"C:\dir\" 里的反斜杠会把引号转义掉）。
    def slashes(path: Path) -> str:
        return str(path).replace("\\", "/")

    lines = [
        "/nologo",
        "/utf-8",
        "/std:c11",
        "/W4",
        f'/Fo"{slashes(OBJ_DIR)}/"',
        f'/Fe:"{slashes(EXE_PATH)}"',
        *[f'/I"{slashes(directory)}"' for directory in INCLUDE_PATHS],
        *[f'"{slashes(source)}"' for source in SOURCE_PATHS],
    ]
    response_path = BUILD_DIR / "compile.rsp"
    response_path.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")

    script = "\r\n".join(
        [
            "@echo off",
            f'call "{vcvars}" >nul',
            "if errorlevel 1 exit /b 1",
            f'cl @"{response_path}"',
            "exit /b %errorlevel%",
        ]
    )
    script_path = BUILD_DIR / "build-host-tests.bat"
    script_path.write_text(script + "\r\n", encoding="utf-8")
    return subprocess.run(["cmd.exe", "/c", str(script_path)], check=False).returncode == 0


def _build_with_posix_compiler(compiler: str) -> bool:
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        *[f"-I{directory}" for directory in INCLUDE_PATHS],
        "-o",
        str(EXE_PATH),
        *[str(source) for source in SOURCE_PATHS],
    ]
    return subprocess.run(command, check=False).returncode == 0


def _pick_compiler() -> tuple[str, Callable[[], bool]] | None:
    """返回 (编译器描述, 编译函数)；本机一个可用的 C 编译器都没有时返回 None。"""
    override = os.environ.get("CC", "").strip()
    if override:
        return override, lambda: _build_with_posix_compiler(override)
    if platform.system() == "Windows":
        vcvars = _find_vcvars()
        if vcvars is not None:
            return f"MSVC ({vcvars})", lambda: _build_with_msvc(vcvars)
    for compiler in ("clang", "gcc", "cc"):
        if shutil.which(compiler) is not None:
            return compiler, lambda: _build_with_posix_compiler(compiler)
    return None


SOURCE_PATHS = [ROOT / item for item in FIRMWARE_SOURCES + TEST_SOURCES]
INCLUDE_PATHS = [ROOT / item for item in INCLUDE_DIRS]


def main() -> int:
    missing = [str(path) for path in SOURCE_PATHS if not path.exists()]
    if missing:
        print("[固件测试] 缺少源文件: " + ", ".join(missing), file=sys.stderr)
        return 1

    picked = _pick_compiler()
    if picked is None:
        print("[固件测试] 找不到 C 编译器。", file=sys.stderr)
        print("[固件测试] Windows 上装 Visual Studio Build Tools 即可；其他平台装 clang 或 gcc。", file=sys.stderr)
        print('[固件测试] 也可以直接指定：$env:CC = "clang"', file=sys.stderr)
        return 1
    description, build = picked

    shutil.rmtree(OBJ_DIR, ignore_errors=True)
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    print("[固件测试] 编译器: " + description)
    if not build():
        print("[固件测试] 编译失败", file=sys.stderr)
        return 1
    return subprocess.run([str(EXE_PATH)], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())

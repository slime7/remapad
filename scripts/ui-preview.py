#!/usr/bin/env python3
"""在开发机上预览屏幕 UI：用 slint-viewer 打开界面，改完存盘即刷新。

用法：
    python scripts/ui-preview.py                                    # 交互预览（设备画面 + 控制条）
    python scripts/ui-preview.py --file ui/src/app.slint            # 只看设备画面（240 × 280，不接动作）
    python scripts/ui-preview.py --check                            # 只编译并打印诊断，不开窗口
    python scripts/ui-preview.py --screenshot agent-temp/ui.png      # 渲染一帧存图后退出

默认打开 ui/preview.slint：上半是 240 × 280 的设备画面（与固件同一棵 AppContent），
下半是控制条，动作按固件语义在预览里结算，因此点设备画面上的控件就能走一遍界面
（动作清单与固件一致，见固件核心 ui_service 的动作分发 firmware/main/ui/ui_service.c）。
预览固定按 1:1 逻辑像素打开，想看放大后的效果就自己设 SLINT_SCALE_FACTOR；
字体与字号表按固件构建的同名环境变量喂给编译器，因此中文与图标与实机同源（烘焙规则见 ui/README.md）。
除 --file 之外的参数原样转给 slint-viewer，完整选项看 slint-viewer --help。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI_DIR = ROOT / "ui"
DEFAULT_SOURCE = UI_DIR / "preview.slint"
DEVICE_SOURCE = UI_DIR / "src/app.slint"

VIEWER = "slint-viewer"
VIEWER_VERSION = "1.18.1"
VIEWER_INSTALL = f"cargo install {VIEWER} --version {VIEWER_VERSION} --locked"
# 与 ui/build-support 的 FONT_SIZES 一致；样式取构件期同款 fluent。
FONT_SIZES = "12,14,16,24"
STYLE = "fluent"

# 仓库统一 UTF-8；管道里按本地代码页输出会让中文变成乱码。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")


def _parse(argv: list[str]) -> tuple[list[str], Path]:
    """摘出 --file，其余参数原样转给 slint-viewer。"""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--file", default=str(DEFAULT_SOURCE))
    known, rest = parser.parse_known_args(argv)
    return rest, Path(known.file).resolve()


def main() -> int:
    viewer_args, source = _parse(sys.argv[1:])
    if shutil.which(VIEWER) is None:
        print(f"[UI 预览] 找不到 {VIEWER}，先安装：{VIEWER_INSTALL}", file=sys.stderr)
        print("[UI 预览] 装好后重开终端让 PATH 生效；也可以用 cargo --list 确认安装位置。", file=sys.stderr)
        return 1
    if not source.exists():
        print(f"[UI 预览] 找不到界面文件：{source}", file=sys.stderr)
        return 1
    if source == DEVICE_SOURCE:
        print("[UI 预览] 只看设备画面：控件动作没有接收方，点按不会切页（交互预览不加 --file）。")

    env = dict(os.environ)
    env.setdefault("SLINT_DEFAULT_FONT", str(UI_DIR / "assets/fonts/NotoSansSC-Regular.otf"))
    env.setdefault("SLINT_FONT_SIZES", FONT_SIZES)
    env.setdefault("SLINT_SCALE_FACTOR", "1")
    command = [VIEWER, "--style", STYLE, *viewer_args, str(source)]
    return subprocess.run(command, env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())

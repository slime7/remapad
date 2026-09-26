"""对固件 C 源码跑 clang-tidy：编译库用 build-clang 目录的 clang 原生副本，诊断直接打到标准输出。

用法：uv run python scripts/clang_tidy.py [路径片段...]（片段按子串过滤文件，不带参数检查全部 .c）。
前置条件：完成一次 clang 构建目录配置，见 docs/GETTING-STARTED.md 的「代码风格与静态检查」；
检查规则与开关在仓库根 .clang-tidy。
"""
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CLANG_BUILD = REPO / "firmware" / "build-clang"
WORKERS = 4


def tracked_c_files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "firmware/main"], capture_output=True, text=True, check=True, cwd=REPO
    ).stdout.split()
    return [REPO / f for f in out if f.endswith(".c")]


def tidy(path: Path) -> str:
    r = subprocess.run(
        ["clang-tidy", "--quiet", "-p", str(CLANG_BUILD), str(path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    out = (r.stdout + r.stderr).strip()
    return out


def main() -> int:
    if not CLANG_BUILD.joinpath("compile_commands.json").exists():
        sys.exit("缺少 firmware/build-clang/compile_commands.json：先按 GETTING-STARTED 配置 clang 构建目录")
    patterns = sys.argv[1:]
    targets = [f for f in tracked_c_files() if any(p in f.as_posix() or p in str(f) for p in patterns)]
    targets = targets or tracked_c_files()
    print(f"clang-tidy: {len(targets)} files, {WORKERS} workers")
    with ThreadPoolExecutor(WORKERS) as ex:
        for path, out in zip(targets, ex.map(tidy, targets)):
            print(f"\n===== {path.relative_to(REPO)} =====")
            print(out or "(clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

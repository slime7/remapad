# 0057 — Python 依赖统一到仓库根一个 uv 工程

- 状态: active
- 日期: 2026-09-24
- 替代: 无

## 背景

根目录 pyproject.toml 只管 scripts/ 且只用标准库，pc/ 另有一套 pyproject.toml + uv.lock + .venv 管 PC 侧工具。
文档里的命令因此分成「仓库根跑 scripts/」与「pc/ 目录跑工具」两种口径。
按路径直接跑 uv run python pc/remapadgui.py 会落到根工程的环境，缺 customtkinter 时直接报 ModuleNotFoundError。
pc/remapadctl.py 的 --image 默认值按工作目录解析，换目录运行会指向别处的镜像。

## 决策

全仓库只保留仓库根一个 uv 工程：pc/ 的第三方依赖（hidapi、customtkinter、av、sounddevice）并入根 pyproject.toml。
pc/pyproject.toml 与 pc/uv.lock 随之删除，环境统一为根目录 .venv。
文档与命令示例一律写成从仓库根出发的 uv run python pc/<工具>.py，在 pc/ 目录里去掉路径前缀同样可用。
pc/remapadctl.py 的默认镜像路径改为按脚本位置解析，与 remapadgui.py 的口径一致。

## 考虑的方案

- 保留两个工程，只把文档写准（否决：按路径直接跑仍落到错的环境，文档与报错对不上）
- 改成 uv workspace，pc/ 作为成员共用根 uv.lock 与 .venv（否决：实测普通 uv run 不安装成员依赖，要先 uv sync --all-packages 才齐，缺一步还是同一个报错）
- 让根工程依赖一个可安装的 pc 包（否决：pc/ 不是要发布的包，为工具加打包机制换不来收益）

## 影响

- 正面：从仓库任意目录 uv run python <路径> 都落到同一个环境，文档只剩一种调用口径。
- 正面：依赖只有一份事实源（根 pyproject.toml + uv.lock），不必再记 pc/ 自己一套。
- 成本：根 .venv 多出四个只有 PC 侧工具用到的依赖（av 与 sounddevice 是二进制轮子）；0040 里「依赖写进 pc/pyproject.toml」一句随本次调整作废。
- 限制：pc/ 不再是独立的 Python 工程，单独拿走 pc/ 时要自带依赖清单。

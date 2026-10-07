# 0065 — PC 侧工具从 Python 迁移到 Node.js

- 状态: active
- 日期: 2026-10-04
- 替代: 0057

## 背景

PC 侧 ctrl、gui 与 MCP 服务原本是仓库根 uv 工程里的 Python 程序，串口免复位打开靠 ctypes 直调 Win32，图形界面用 CustomTkinter。

## 决策

PC 侧工具整体迁到 pnpm workspace 管理的 Node.js（Node 22 ESM）。
串口经 koffi 直调 Win32 保持免复位语义，蓝牙触觉用 @discordjs/opus 与 audify，HID 用 node-hid，MCP 用 @modelcontextprotocol/sdk。
图形界面改 Node 后端加 Vue 3 与 mde-vue 前端，测试整体搬到 vitest 并与实现同步移植。

## 考虑的方案

- 继续维护 Python 版或引入 Rust 重写：Python 版动态类型在帧协议与多线程收发上易出静默错位，Rust 会显著抬高参与门槛；Node.js 类型边界虽弱但单线程事件模型贴近会话循环，生态里四类原生能力都有成熟预编译包。

## 影响

- 仓库根不再有 uv 工程与 pc/tests，宿主用例集中在 pc/test 由根目录 vitest 运行；scripts/ 下与固件、ADR 相关的 Python 脚本保留原样，GUI 需要 Node 22 与 pnpm 安装与构建。

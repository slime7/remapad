# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的 ESP32-S3 (N16R8) 的嵌入式控制器系统，最终目标是“USB 输入 → NS2 手柄报告 → BLE 手柄”，并配套 PocketJS 屏幕 UI。工程采用“PocketJS 前端 (Vue Vapor + Tailwind) + ESP-IDF 固件”双工作区架构；NS2/BLE 协议资料见 [docs/controller.md](docs/controller.md)。

## 开始任务前必读

在参与本项目的设计、编码、审查或重构任务前，必须阅读以下项目文档：

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)：明确项目定位、目标受众与非目标。
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)：掌握双工作区组成、数据流与构建流水线。
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：掌握 PocketJS 节点模型、Tailwind 编译机制与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：掌握开发环境搭建、常用命令与调试排错方法。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：查阅具有长期影响的既定架构决策与选型取舍。
6. [控制器协议参考 (docs/controller.md)](docs/controller.md)：掌握 USB→NS2→BLE 数据面的协议范围、配对和广播验证边界。

## 项目工程架构与工作区划分

项目划分为两个独立但紧密协同的工作区：

- **前端 UI 工程 (`ui/`)**：
  - 基于 PocketJS 框架与 Vue 3 Vapor JSX 语法构建。
  - 样式使用 PocketJS 构建期 Tailwind CSS 子集，字体由构建器光栅化烘焙。
  - 依赖由 pnpm 管理，官方 PocketJS 编译器由 Bun 执行。
- **设备固件工程 (`firmware/`)**：
  - 基于 PocketJS 官方要求的 ESP-IDF `>=6.0,<6.2` 与 C 语言编写。
  - 硬件绑定 ESP32-S3-WROOM-1 N16R8（16MB Flash + 8MB Octal PSRAM）。
  - 通过官方 `pocketjs_*` ESP-IDF 组件嵌入或编译 `.pocket` 包；六个组件与 ESP32-S3 原生归档固定在 `firmware/components/`，由 ESP-IDF 默认发现，构建不依赖 PocketJS checkout，也不要把它改回外部路径。产品固件还负责 USB 接收、NS2 报告转换、BLE 广播/GATT/配对和显示提交。
  - `main/bridge/`、`main/drivers/` 和 `ui/src/bridge/` 是未来产品控制面/数据面的预留代码，即使当前未编译或未被 UI 调用，也不要仅因暂时未使用而删除。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **依赖安装** | `pnpm install` | 安装前端工作区依赖 |
| **代码检查** | `pnpm run lint` | 执行前端 ESLint 静态代码检查 |
| **PocketJS 契约检查** | `pnpm run check` | 使用官方 CLI 和 `firmware/pocket.host.json` 校验清单、能力与视口 |
| **前端资源编译** | `pnpm run compile` | 调用官方 PocketJS 编译器，输出 `.js` 与 `.pak` |
| **前端应用打包** | `pnpm run build` | 调用官方 `pocket build --host-profile` 输出 `.pocket` |
| **原生归档重建** | `pnpm run native` | 仅在升级组件时用官方 `tools/esp-idf-native.ts` 重新生成 `firmware/components/` 内的 `libpocketjs_idf_ui_core.a` 与 `libpocketjs_idf_render_rgb565.a` |
| **上游对账** | 见 [patches/README.md](patches/README.md) | 升级 `firmware/components/` 后核对 QuickJS 源码校验值与 `build-receipt.json` |
| **触摸预览** | `pnpm run dev` | 编译后启动项目内的触摸屏预览页（端口 8130，240 × 280，触摸输入，无实体按键） |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件 |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器 |

## 产物与生成文件约定

- **禁止手动修改构建产物**：
  - `ui/dist/` 为前端构建产物目录，由构建脚本全自动生成。
  - `firmware/build/` 为 ESP-IDF 编译目录。
  - `ui/.pocket/` 为 PocketJS 计划文件目录。
- `.pocket` 包由官方 PocketJS CLI 生成；ESP-IDF 的 `pocketjs_embed_package` 会在 `firmware/build/` 中生成临时 C/汇编嵌入文件，禁止提交或手动维护。

## 项目特有约束

- **字体烘焙规则**：
  - PocketJS 不依赖宿主操作系统字体，新增文本的字号应使用 Tailwind 支持的标准插槽（如 `text-xs`, `text-sm`, `text-base`, `text-lg`, `text-xl`）。
  - 构建期会自动提取文本字符集并在烘焙阶段生成对应插槽的点阵图集。

- **PocketJS 组件、归档与脚本入口**：
  - 仓库是自包含的：`firmware/components/` 固定官方 ESP-IDF 组件与 ESP32-S3 原生归档，`ui/node_modules/@pocketjs/framework` 提供 Web 开发主机（npm 上的 0.11.0 还不含 ESP-IDF host profile 编译器，该编译器来自可选 checkout）。
  - `POCKETJS_ROOT` 是可选的对照路径，只在 npm 版本缺少 host profile 编译器或需要重建原生归档时使用；不要把本项目的产物写进该目录。
  - 硬件屏幕是触摸屏：`ui/preview/` 是项目自己的预览页，把浏览器触摸事件转换为 PocketJS 触摸帧契约（`frame(buttons, analog, touches, hits)`），由 `scripts/preview-server.mjs` 提供静态服务；不要再退回官方 playground 的 PSP 按键界面。
  - 升级 `firmware/components/` 后必须重新生成原生归档并核对 QuickJS 校验值，见 [patches/README.md](patches/README.md)。

## 文档维护触发映射

文档栏目可能持续增加。修改文档时应优先保持现有标题层级与内容顺序，仅在确有必要表达层级或便于导航时使用序号标题；新增内容尽量就地追加或局部修改，避免无关的重排、重编号和大面积 diff。

| 变更范围 | 应同步维护的文档 |
| :--- | :--- |
| 硬件规格、屏幕驱动、Flash/PSRAM 配置变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) |
| 产品定位、服务受众、非目标边界变动 | [docs/VISION.md](docs/VISION.md) |
| 跨层数据协议、核心图元、宏常量与状态模型变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| USB 输入、NS2 报告、BLE 广播/GATT、配对或绑定状态变动 | [docs/controller.md](docs/controller.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

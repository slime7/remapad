# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的 ESP32-S3 (N16R8) 的嵌入式 UI 开发系统，采用“PocketJS 前端 (Vue Vapor + Tailwind) + ESP-IDF 固件”双工作区架构。

## 开始任务前必读

在参与本项目的设计、编码、审查或重构任务前，必须阅读以下项目文档：

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)：明确项目定位、目标受众与非目标。
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)：掌握双工作区组成、数据流与构建流水线。
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：掌握 PocketJS 节点模型、Tailwind 编译机制与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：掌握开发环境搭建、常用命令与调试排错方法。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：查阅具有长期影响的既定架构决策与选型取舍。

## 项目工程架构与工作区划分

项目划分为两个独立但紧密协同的工作区：

- **前端 UI 工程 (`ui/`)**：
  - 基于 PocketJS 框架与 Vue 3 Vapor JSX 语法构建。
  - 样式使用 PocketJS 构建期 Tailwind CSS 子集，字体由构建器光栅化烘焙。
  - 依赖与脚本由 Bun 及 pnpm 驱动。
- **设备固件工程 (`firmware/`)**：
  - 基于乐鑫官方 ESP-IDF (v5.3+) 框架与 C 语言编写。
  - 硬件绑定 ESP32-S3-WROOM-1 N16R8（16MB Flash + 8MB Octal PSRAM）。
  - 静态编译包含前端自动导出的头文件并直接烧录至开发板。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **依赖安装** | `pnpm install` | 安装前端工作区依赖 |
| **代码检查** | `pnpm run lint` | 执行前端 ESLint 静态代码检查 |
| **代码修复** | `pnpm run lint:fix` | 自动修复前端代码格式问题 |
| **前端打包** | `cd ui ; bun run build` | 编译 JSX、光栅化样式与字体、输出 `.pocket` 并同步固件头文件 |
| **本地仿真** | `cd ui ; bun run dev` | 启动浏览器 60 FPS WebAssembly 仿真服务器 (端口 8130，支持热重载) |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件 |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器 |

## 产物与生成文件约定

- **禁止手动修改构建产物**：
  - `ui/dist/` 为前端构建产物目录，由构建脚本全自动生成。
  - `firmware/build/` 为 ESP-IDF 编译目录。
- **固件头文件单向同步**：
  - [firmware/main/app_pocket.h](firmware/main/app_pocket.h) 为前端构建生成的 C 静态常量数组，由前端 `build.mjs` 全自动同步覆盖，禁止在该文件中手动书写业务代码。

## 项目特有约束

- **字体烘焙规则**：
  - PocketJS 不依赖宿主操作系统字体，新增文本的字号应使用 Tailwind 支持的标准插槽（如 `text-xs`, `text-sm`, `text-base`, `text-lg`, `text-xl`）。
  - 构建期会自动提取文本字符集并在烘焙阶段生成对应插槽的点阵图集。

## 文档维护触发映射

文档栏目可能持续增加。修改文档时应优先保持现有标题层级与内容顺序，仅在确有必要表达层级或便于导航时使用序号标题；新增内容尽量就地追加或局部修改，避免无关的重排、重编号和大面积 diff。

| 变更范围 | 应同步维护的文档 |
| :--- | :--- |
| 硬件规格、屏幕驱动、Flash/PSRAM 配置变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) |
| 产品定位、服务受众、非目标边界变动 | [docs/VISION.md](docs/VISION.md) |
| 跨层数据协议、核心图元、宏常量与状态模型变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8）的嵌入式控制器系统，最终目标是“USB 输入 → NS2 手柄报告 → BLE 手柄”，并配套 PocketJS 屏幕 UI。工程采用“PocketJS 前端 (Vue Vapor + Tailwind) + ESP-IDF 固件”双工作区架构；NS2/BLE 协议资料见 [docs/controller.md](docs/controller.md)，板卡规格见 [docs/hardware.md](docs/hardware.md)。

## 开始任务前必读

在参与本项目的设计、编码、审查或重构任务前，必须阅读以下项目文档：

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)：明确项目定位、目标受众与非目标。
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)：掌握双工作区组成、数据流与构建流水线。
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：掌握 PocketJS 节点模型、Tailwind 编译机制与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：掌握开发环境搭建、常用命令与调试排错方法。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：查阅具有长期影响的既定架构决策与选型取舍。
6. [控制器协议参考 (docs/controller.md)](docs/controller.md)：掌握 USB→NS2→BLE 数据面的协议范围、配对和广播验证边界。
7. [目标硬件参考 (docs/hardware.md)](docs/hardware.md)：掌握目标板卡的 SoC/存储、屏幕与触摸器件、外设地址、GPIO 分配和板级注意事项。

## 项目工程架构与工作区划分

项目划分为两个独立但紧密协同的工作区：

- **前端 UI 工程 (`ui/`)**：
  - 基于 PocketJS 框架与 Vue 3 Vapor JSX 语法构建。
  - 样式使用 PocketJS 构建期 Tailwind CSS 子集，字体由构建器光栅化烘焙。
  - 依赖由 pnpm 管理，PocketJS 编译器由 Bun 执行，编译器与框架来源为仓库内的 `ui/vendor/pocketjs` 快照。
  - 页面由 `ui/src/App.tsx` 组织：首帧只挂壳、状态栏、底栏与首页，其余页面首次进入时才挂载，并在页内用 `ui/src/hooks/useProgressiveMount.ts` 逐帧自上而下填充（见 [ADR 0014](docs/adr/0014-page-mount-on-demand-progressive-fill.md)）；新增页面必须登记到 App 的挂载分支、页内分块，并由页面根节点自己翻转 `hidden`。
- **设备固件工程 (`firmware/`)**：
  - 基于 PocketJS 官方要求的 ESP-IDF `>=6.0,<6.2` 与 C 语言编写。
  - 硬件绑定微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8，16MB Flash + 8MB Octal PSRAM，240×280 ST7789V2 触摸屏）；规格与引脚见 [docs/hardware.md](docs/hardware.md)。
  - QuickJS guest 的创建、mount、eval 与逐帧 UI turn 必须由同一个任务承载，且该任务栈要大于 guest 的 `stack_limit`；当前由 `firmware/main/pocketjs_host.c` 的 `remapad-pjs` owner task 承担（栈在 PSRAM）。改动调度或栈预算前先读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 的“为什么由产品 task 承载 guest 生命周期”。
  - 通过官方 `pocketjs_*` ESP-IDF 组件嵌入或编译 `.pocket` 包；六个组件与 ESP32-S3 原生归档固定在 `firmware/components/`，由 ESP-IDF 默认发现，构建不依赖 PocketJS checkout，也不要把它改回外部路径。产品固件还负责 USB 接收、NS2 报告转换、BLE 广播/GATT/配对和显示提交。
  - `main/` 下的 `bridge/`（控制面命令/事件，PWR 按键与串口 CLI 经外部队列汇入）、`config/`（NVS 用户设置持久化）、`console/`（串口 CLI）、`dp/`（数据面任务与输入源抽象）、`ns2/`（NS2 编码、序列号命名规则与输出封装）、`ble/`（NimBLE 手柄外设、双身份会话与分槽凭证）、`drivers/`（panel/touch/backlight/pwr_key/buzzer/battery）和顶层 `boot_splash.c`（UI 就绪前的启动画面，随面板启动点亮背光）都已编译进固件；模块边界见 [docs/adr/0011](docs/adr/0011-controller-dataplane-module-boundary.md)，USB 输入与桥接模式仍是架构预留（方案预案见 [docs/usb-input-plan.md](docs/usb-input-plan.md)），新增输入设备按 `dp/dp_source.h` 的输入源接口注册，不要绕过它直连编码器。

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
| **触摸预览** | `pnpm run dev` | 编译后启动项目内的触摸屏预览页（端口 8130，240 × 280，触摸输入，无实体按键），并同时拉起官方 DevTools 服务器（面板 8131） |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件 |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器；禁止对已写入用户数据的设备执行 `erase-flash`（会清空 NVS 设置/配对与 `storage` 分区，布局约束见 [ADR 0009](docs/adr/0009-ota-storage-flash-layout.md)） |
| **固件增量烧录** | `cd firmware ; idf.py -p COMx app-flash` | 仅重写应用分区（`ota_0` @ 0x10000）；改动 bootloader/分区表后仍需完整烧录 |

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
  - 中文等 Inter 未映射的码点由官方 `fonts.json` 回退机制解析：`ui/src/fonts.json` 把 `ui/assets/fonts/NotoSansSC-Regular.otf`（SIL OFL 1.1，许可文本同目录）声明为回退字体面，Inter/JetBrains Mono 仍负责各自槽位的拉丁字形。回退清单对所有槽位只有一份，中文粗体实际烘焙为常规字重。
  - 字符集来自构建期对源码字符串字面量、模板字符串静态块和 JSX 文本的静态扫描；只在运行时动态拼接、且从未出现在任何字面量里的字符不会被烘焙。字体未映射的码点（如 emoji）没有字形，渲染为 tofu 方框。

- **PocketJS 组件、归档与脚本入口**：
  - 仓库是自包含的：`firmware/components/` 固定官方 ESP-IDF 组件与 ESP32-S3 原生归档，`ui/vendor/pocketjs` 固定编译器、框架源码、构建资源与触摸预览用的官方 wasm 核心。
  - `ui/vendor/pocketjs/framework/src/styles.generated.ts` 是编译器生成的样式镜像，但必须随快照提交：官方类型检查跑在编译器写入它之前，且 `pnpm install` 之后新增的快照文件不会进入依赖副本。它按 `ui/src` 重新生成，出现差异时直接提交。
  - `POCKETJS_ROOT` 是可选的对照路径，只在重新生成快照（`scripts/vendor-pocketjs.mjs`）或重建原生归档时使用；不要把本项目的产物写进该目录。
  - 硬件屏幕是触摸屏：`ui/preview/` 是项目自己的预览页，把浏览器触摸事件转换为 PocketJS 触摸帧契约（`frame(buttons, analog, touches, hits)`），由 `scripts/preview-server.mjs` 提供静态服务；不要再退回官方 playground 的 PSP 按键界面。预览页按官方 `engine.js` 的设备协议接入快照内携带的官方 DevTools 服务器（`hosts/web/serve.ts`，面板 + WebSocket hub）；官方 playground 没有触摸输入，不要用它替代触摸预览页。
  - 升级 `firmware/components/` 后必须重新生成原生归档并核对 QuickJS 校验值，见 [patches/README.md](patches/README.md)。

## 文档维护触发映射

文档栏目可能持续增加。修改文档时应优先保持现有标题层级与内容顺序，仅在确有必要表达层级或便于导航时使用序号标题；新增内容尽量就地追加或局部修改，避免无关的重排、重编号和大面积 diff。

| 变更范围 | 应同步维护的文档 |
| :--- | :--- |
| 硬件规格、屏幕驱动、Flash/PSRAM 配置变动 | [docs/hardware.md](docs/hardware.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) |
| 板卡外设、GPIO 分配、总线地址变动 | [docs/hardware.md](docs/hardware.md) |
| 产品定位、服务受众、非目标边界变动 | [docs/VISION.md](docs/VISION.md) |
| 跨层数据协议、核心图元、宏常量与状态模型变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| USB 输入、NS2 报告、BLE 广播/GATT、配对或绑定状态变动 | [docs/controller.md](docs/controller.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8）的嵌入式控制器系统：
USB 输入 → NS2 手柄报告 → BLE 手柄，配套 PocketJS 屏幕 UI。
工程分为 `ui/`（PocketJS 前端，Vue Vapor + Tailwind）与 `firmware/`（ESP-IDF 固件）两个工作区；
NS2/BLE 协议资料见 [docs/controller.md](docs/controller.md)，板卡规格见 [docs/hardware.md](docs/hardware.md)。

## 名词约定

文档、代码注释与提交信息里的几组称呼按下面含义使用，不要混用：

- **目标主机**：接收本设备手柄报告的那台游戏机；`主机`、`游戏机`、`NS`、`Switch`、`NS2`、`Switch 2` 都指它。
  需要区分型号时写全称（如 `NS2 主机`），泛指协议行为时用 `主机`。
- **本硬件**：运行 Remapad 固件的这块板卡；`ESP`、`板子`、`ESP32`、`ESP32-S3` 都指它（对外型号为微雪 ESP32-S3-Touch-LCD-1.69）。
- **连接键**：用户按下就开广播的那颗键——配对页的「连接」按钮与 PWR 长按 3 秒是同一个动作；
  它在未配对身份上等价于真机的配对键，因此也叫 `配对键`。
- **手柄身份**：本设备对外呈现的手柄（`Pro` 或 `JoyCon 组合`），与「目标主机」不是一回事；
  指 USB 直插的实体手柄时写 `输入设备` 或 `USB 手柄`。

## 开始任务前必读

在参与本项目的设计、编码、审查或重构任务前，必须阅读以下项目文档：

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)：项目定位、目标受众与非目标。
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)：双工作区组成、数据流与构建流水线。
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：PocketJS 节点模型、Tailwind 编译机制与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：环境搭建、常用命令与调试排错。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：既定架构决策与选型取舍。
6. [控制器协议参考 (docs/controller.md)](docs/controller.md)：USB→NS2→BLE 数据面协议、配对与广播验证边界。
7. [目标硬件参考 (docs/hardware.md)](docs/hardware.md)：SoC/存储、屏幕与触摸器件、外设地址、GPIO 分配与板级注意事项。
8. [测试策略与回归规则 (docs/TESTING.md)](docs/TESTING.md)：测试运行方式、断言分层与「先写用例再修 bug」规则。

## 项目工程架构与工作区划分

- **前端 UI 工程 (`ui/`)**：
  - 基于 PocketJS 框架与 Vue 3 Vapor JSX 语法；样式用 PocketJS 构建期 Tailwind CSS 子集，字体由构建器光栅化烘焙。
  - 依赖由 pnpm 管理，PocketJS 编译器由 Bun 执行，来源为仓库内 `ui/vendor/pocketjs` 快照。
  - 页面由 `ui/src/App.tsx` 组织：首次渲染一次性挂载全部七个页面，切页只翻转各页根节点的 `hidden`，新增页面直接写进 JSX
    （[ADR 0016](docs/adr/0016-mount-all-pages-before-first-frame.md)）。
- **设备固件工程 (`firmware/`)**：
  - 基于 PocketJS 官方要求的 ESP-IDF `>=6.0,<6.2` 与 C 语言；硬件绑定微雪 ESP32-S3-Touch-LCD-1.69
    （16MB Flash + 8MB Octal PSRAM，240×280 ST7789V2 触摸屏），规格与引脚见 [docs/hardware.md](docs/hardware.md)。
  - QuickJS guest 的创建、mount、eval 与逐帧 UI turn 必须由同一个任务承载，且任务栈要大于 guest 的 `stack_limit`；
    当前由 `firmware/main/pocketjs_host.c` 的 `remapad-pjs` owner task 承担（栈在 PSRAM），
    改动调度或栈预算前先读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 的「为什么由产品 task 承载 guest 生命周期」。
  - 六个官方 `pocketjs_*` 组件与 ESP32-S3 原生归档固定在 `firmware/components/`，由 ESP-IDF 默认发现，构建不依赖 PocketJS checkout；
    产品固件负责 USB 接收、NS2 报告转换、BLE 广播/GATT/配对和显示提交。
- **`firmware/main/` 模块**：
  - `bridge/`：控制面命令/事件，PWR 按键与串口 CLI 经外部队列汇入。
  - `config/`：NVS 用户设置持久化；setter 只置内存表脏标记，提交任务每 1 分钟检查一次，确有改动才写一次 NVS。
  - `console/`：串口 CLI 与控制台出口切换；切到 USB host 后日志与 CLI 走 UART0。
  - `dp/`：数据面任务、输入源抽象与组合键捕获屏幕（`dp_ui.c`，[ADR 0028](docs/adr/0028-pad-combo-captures-screen.md)）；
    数据面汇合约定见 [ADR 0011](docs/adr/0011-controller-dataplane-module-boundary.md)。
  - `input/`：输入通路接收段：桥接帧协议、USB-Serial/JTAG 唯一读取者、桥接输入源。
    三段边界见 [ADR 0021](docs/adr/0021-input-path-three-stage-layering.md)，
    同代透传规则见 [ADR 0026](docs/adr/0026-same-generation-input-passthrough.md)。
  - `usb/`：USB host 直插：枚举与 HID 收发、输入源、运行时角色切换。
    方案与实机核对清单见 [docs/usb-input-plan.md](docs/usb-input-plan.md)，
    取舍见 [ADR 0027](docs/adr/0027-runtime-usb-role-switch.md)。
  - `ota/`：升级会话：非运行分区回写、窗口流控与回滚健康门槛（[ADR 0022](docs/adr/0022-ota-over-bridge-frames-with-rollback.md)）。
  - `pad/`：处理段：私有格式 `pad_state_t`、解析与归一、按布局行编码的反馈；
    家族布局表按系列拆在 `pad/layouts/`，契约与注册表是 `pad/layout.h` / `pad/layout.c`。
  - `target/`：转换段：目标编码接口 `pad_target_t`；`target/ns2/` 负责 NS2 编码、序列号命名规则与输出封装。
  - `ble/`：NimBLE 手柄外设、双身份会话与分槽凭证。
  - `drivers/`：panel / touch / backlight / pwr_key / buzzer / battery；
    显示通路条带划分与刷新取值见 [ADR 0017](docs/adr/0017-display-path-and-scroll-frame-budget.md)。
  - 顶层 `boot_splash.c`：UI 就绪前的启动画面，随面板启动点亮背光；`render_accel.c`：S3 上接管渲染器填充/掩码混合/直拷回调的本机实现。
  - PC 侧程序在 `pc/`（`remapadctl.py`：转发 + 命令行 + 实机截图 + OTA；`remapadgui.py`：同一套会话的图形界面），见 [pc/README.md](pc/README.md)。
  - 新增输入设备按 `dp/dp_source.h` 的输入源接口注册，不要绕过它直连编码器。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **依赖安装** | `pnpm install` | 安装前端工作区依赖 |
| **代码检查** | `pnpm run lint` | 前端 ESLint 静态检查 |
| **UI 端到端测试** | `pnpm run test:e2e` | Playwright 驱动触摸预览页里的真实产物，断言页面行为与屏幕像素；`test:e2e:headed` 可看过程，规则见 [docs/TESTING.md](docs/TESTING.md) |
| **固件主机端测试** | `pnpm run test:firmware` | 把与硬件无关的固件逻辑编译成开发机可执行文件并运行，秒级出结果 |
| **PC 侧主机端测试** | `pnpm run test:pc` | `pc/` 工具里与设备无关的纯逻辑（串口枚举、镜像校验、帧编解码、输出分流、工具命令解析）在 `pc/tests/` 用标准库 unittest 跑，不接设备 |
| **PocketJS 契约检查** | `pnpm run check` | 官方 CLI + `firmware/pocket.host.json` 校验清单、能力与视口 |
| **前端资源编译** | `pnpm run compile` | 官方 PocketJS 编译器输出 `.js` 与 `.pak`（默认 dev 状态，包含调试页） |
| **前端应用打包** | `pnpm run build` | 官方 `pocket build --host-profile` 输出 `.pocket`（默认 dev 状态）；正式发布使用 `pnpm run build:release`（剔除调试页） |
| **原生归档重建** | `pnpm run native` | 仅升级组件时重新生成 `firmware/components/` 内的两个 `.a` |
| **上游对账** | 见 [patches/README.md](patches/README.md) | 升级 `firmware/components/` 后核对 QuickJS 校验值与 `build-receipt.json` |
| **触摸预览** | `pnpm run dev` | 编译并启动触摸预览页（端口 8130，240 × 280，触摸输入），同时拉起官方 DevTools 服务器（面板 8131） |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件 |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器；禁止对已写入用户数据的设备执行 `erase-flash`（会清空 NVS 设置/配对与 `storage` 分区，见 [ADR 0009](docs/adr/0009-ota-storage-flash-layout.md)） |
| **固件增量烧录** | `cd firmware ; idf.py -p COMx app-flash` | 仅重写应用分区（`ota_0` @ 0x10000）；改动 bootloader/分区表后仍需完整烧录 |
| **固件 OTA 升级** | `cd pc ; uv run python remapadctl.py -p COMx --upgrade` | 经 USB-Serial/JTAG 推送 `firmware/build/remapad_firmware.bin`（含内嵌 `.pocket`）到非运行分区，校验通过后自动重启；`--dry-run` 只校验镜像、`--wait` 等设备回来后打印版本；从 `ota_1` 启动后继续开发要先 `idf.py erase-otadata` |
| **PC 手柄桥接** | `cd pc ; uv run python remapadctl.py -p COMx` | 读 PC 手柄原始报告按桥接帧转发给设备，同进程提供串口命令行、实机截图与 OTA；`--list` 枚举手柄、`--dump` 抓原始报告核对家族表偏移；转发默认只在交互模式开，`--pad` / `--no-pad` 控制 |
| **PC 连接控制台** | `cd pc ; uv run python remapadgui.py` | 同一套会话的图形界面：选串口、连接/断开、手柄转发开关、实时日志、命令输入、实机截图与 OTA；与命令行不要同时连同一个口 |
| **串口 CLI** | `cd pc ; uv run python remapadctl.py -p COMx status` | 行命令控制台：位置参数透传设备命令、`--log` 只读日志、交互模式 `:help` 看工具命令；常用设备命令有 `link`、`headset`、`shot`、`key ui` 与 `ui on\|off`、`version`、`rollback` |
| **实机截图** | `cd pc ; uv run python remapadctl.py -p COMx --shot` | 固件把当前画面整屏重渲染并按图像帧回传，PC 拼成 PNG（默认 `pc/shots/`，`--out` 指定路径；期间 UI 冻结约 0.2-1 秒，见 [ADR 0033](docs/adr/0033-pc-single-process-tool-and-device-screenshot.md)） |

固件命令要在**配置本工程时用的那套 ESP-IDF 环境**里执行；配置用的解释器记录在 `firmware/build/CMakeCache.txt`
（`rg -n '^PYTHON' firmware/build/CMakeCache.txt`）。
切到另一套 IDF 环境时 `idf.py` 只打印环境提示就返回、不编译；判据与排错见 [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) 的「6. 编译 ESP-IDF 固件」。

## 产物与生成文件约定

- 禁止手动修改构建产物：`ui/dist/`、`firmware/build/`、`ui/.pocket/` 均由构建脚本全自动生成。
- `.pocket` 包由官方 PocketJS CLI 生成；`pocketjs_embed_package` 在 `firmware/build/` 生成的临时 C/汇编嵌入文件禁止提交。
- 项目相关的临时文件（脚本、抓包与 `--dump` 输出、截图、日志、一次性分析产物）一律放 `agent-temp/`（`.gitignore` 已忽略其内容，只有 `.gitkeep` 入库）；
  要长期保留的东西再按各自目录约定落位。

## 项目特有约束

- **连接由用户发起**：上电与断连（主机睡下）都静默，只有连接键（配对页「连接」、PWR 长按 3 秒）打开连接窗口、
  未连接时按手柄 HOME（实体手柄按下去或调试页注入）打开唤醒窗口把休眠主机叫起来；窗口到期或主机连上即收窗。
  改这条策略前先读 [ADR 0038](docs/adr/0038-user-initiated-connection-window.md)。
- **缺陷修复先写用例**：改 UI 的 bug 先在 `ui/tests/e2e/` 加一条能复现的红用例，改完 `ui/src` 后转绿才算修完；
  固件里与硬件无关的逻辑缺陷同样先补 `firmware/test/` 的主机端用例。
  用例标题写用户看到的现象，不放宽断言迁就实现，规则见 [docs/TESTING.md](docs/TESTING.md)。
- **测试只用真源码**：端到端测试跑 `ui/dist` 真实产物与官方 wasm 渲染核心，固件测试编译 `firmware/main/` 下的源码；
  `firmware/test/support/stubs/` 只补齐主机缺失的 ESP-IDF 头文件与硬件取值入口，不得把被测逻辑复制一份进测试。
- **手柄操控屏幕模式**：手柄按 L1+R1+L3+R3（按住 300 ms）捕获输入，此后只向主机续发全松开的中性帧、玩家按键不再上行（整段停发会被主机判离线），
  改为方向键移动焦点、圆圈键确认（[ADR 0028](docs/adr/0028-pad-combo-captures-screen.md)）；
  Web 触摸预览页不需要组合键，方向键 / WASD 与 Enter / 空格直接驱动同一套按键位，实机用 `key ui` 或 `ui on\|off` 进出。
  新增可点控件要带 `focus:` 环（写在 `ui/src/theme.ts` 的 className 字面量里，构建期按字面量登记样式）；
  `focusable` 必须绑 App 传下来的 `interactive()`，静态 `focusable` 会把隐藏页控件留在名单里；
  可滚动页新增可聚焦行时把行位置加进 [ui/src/hooks/usePageScroll.ts](ui/src/hooks/usePageScroll.ts) 的 `focusRows`。
- **字体烘焙规则**：
  - 文本字号用 Tailwind 标准插槽（`text-xs` 等）；构建期扫描源码字面量字符集，按插槽烘焙点阵图集。
  - 中文等 Inter 未映射码点由 `ui/src/fonts.json` 声明的 NotoSansSC 回退面解析（中文粗体实际烘焙为常规字重）。
  - 只在运行时动态拼接、从未出现在字面量里的字符不会被烘焙；字体未映射的码点（如 emoji）渲染为 tofu 方框。
  - 界面文案必须写在 `ui/src` 里：固件经 bridge 回发的文本不会被烘焙，直接上屏显示成豆腐块。
- **PocketJS 组件、归档与脚本入口**：
  - 仓库自包含：`firmware/components/` 固定官方 ESP-IDF 组件与 ESP32-S3 原生归档，`ui/vendor/pocketjs` 固定编译器、框架源码与触摸预览用的 wasm 核心。
  - `ui/vendor/pocketjs/framework/src/styles.generated.ts` 必须随快照提交（官方类型检查跑在编译器写入它之前，且 `pnpm install` 后新增的快照文件不进依赖副本）；
    它按 `ui/src` 重新生成，出现差异直接提交。
  - `POCKETJS_ROOT` 只在重新生成快照（`scripts/vendor-pocketjs.mjs`）或重建原生归档时用作对照路径，不要把本项目产物写进去。
  - 触摸预览一律用 `ui/preview/`（浏览器触摸事件 → PocketJS 触摸帧，官方 playground 无触摸输入）。
  - 升级 `firmware/components/` 后必须重新生成原生归档并核对 QuickJS 校验值，见 [patches/README.md](patches/README.md)。
- **单行不超过 120 字符**：`.editorconfig` 的 `max_line_length = 120` 适用于代码、脚本与文档；Markdown 正文按句子断行，一句一行，整句过长就改短。
  上游快照（`ui/vendor/pocketjs/`、`firmware/components/`）、`patches/*.patch`、锁文件与单行 SVG 豁免；
  GFM 表格行（单元格不能折行）与必须整行粘贴执行的命令保持原样。

## 文档维护触发映射

修改文档时保持现有标题层级与内容顺序，新增内容就地追加或局部修改，避免无关的重排、重编号与大面积 diff。

| 变更范围 | 应同步维护的文档 |
| :--- | :--- |
| 硬件规格、屏幕驱动、Flash/PSRAM 配置变动 | [docs/hardware.md](docs/hardware.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) |
| 板卡外设、GPIO 分配、总线地址变动 | [docs/hardware.md](docs/hardware.md) |
| 产品定位、服务受众、非目标边界变动 | [docs/VISION.md](docs/VISION.md) |
| 跨层数据协议、核心图元、宏常量与状态模型变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| USB 输入、NS2 报告、BLE 广播/GATT、配对或绑定状态变动 | [docs/controller.md](docs/controller.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 输入通路（桥接帧协议、私有格式、家族表、目标编码）变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md), [docs/usb-input-plan.md](docs/usb-input-plan.md), [pc/README.md](pc/README.md) |
| 显示通路的条带划分/整幅刷新取值、滚动帧预算或面板时钟变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/adr/0017](docs/adr/0017-display-path-and-scroll-frame-budget.md), [docs/adr/0018](docs/adr/0018-panel-spi2-clock-80mhz.md) |
| OTA 升级通路、桥接帧类型或载荷布局变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), [pc/README.md](pc/README.md), [docs/adr/0022](docs/adr/0022-ota-over-bridge-frames-with-rollback.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 测试入口、用例范围、回归规则或断言分层变动 | [docs/TESTING.md](docs/TESTING.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

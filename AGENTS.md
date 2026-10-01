# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8）的嵌入式控制器系统：
USB 输入 → NS2 手柄报告 → BLE 手柄，配套屏幕 UI。
工程分为 `ui/`（屏幕 UI 工作区：界面源码、固件界面组件与宿主用例）与 `firmware/`（ESP-IDF 固件核心）两个工作区；
主机协议资料见 [docs/controller-switch2.md](docs/controller-switch2.md)，输入设备数据见 [docs/controller-ps.md](docs/controller-ps.md)，
板卡规格见 [docs/hardware.md](docs/hardware.md)。

## 名词约定

文档、代码注释与提交信息里的几组称呼按下面含义使用，不要混用：

- **目标主机**：接收本设备手柄报告的那台游戏机；`主机`、`游戏机`、`NS`、`Switch`、`NS2`、`Switch 2` 都指它。
  需要区分型号时写全称（如 `NS2 主机`），泛指协议行为时用 `主机`。
- **本硬件**：运行 Remapad 固件的这块板卡；`ESP`、`板子`、`ESP32`、`ESP32-S3` 都指它（对外型号为微雪 ESP32-S3-Touch-LCD-1.69）。
- **连接键**：用户按下就开广播的那颗键——配对页的「连接」按钮与 PWR 长按 3 秒是同一个动作；
  它在未配对身份上等价于真机的配对键，因此也叫 `配对键`。
- **手柄身份**：本设备对外呈现的手柄（现役只有 `Pro`），与「目标主机」不是一回事；
  指 USB 直插的实体手柄时写 `输入设备` 或 `USB 手柄`。

## 开始任务前必读

在参与本项目的设计、编码、审查或重构任务前，必须阅读以下项目文档：

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)
6. [Switch 2 手柄协议规范 (docs/controller-switch2.md)](docs/controller-switch2.md)
7. [PS 家族手柄数据规范 (docs/controller-ps.md)](docs/controller-ps.md)
8. [目标硬件参考 (docs/hardware.md)](docs/hardware.md)
9. [测试策略与回归规则 (docs/TESTING.md)](docs/TESTING.md)
10. [屏幕 UI 工作区 (ui/README.md)](ui/README.md)

## 项目工程架构与工作区划分

- **屏幕 UI 工程 (`ui/`)**：
  - `ui/src/`：界面根组件、页面与复用控件。
  - `ui/host/`：宿主侧测试用例包。
  - `ui/slint_ui/`：固件界面组件与平台适配。
  - `ui/preview/`：WASM 预览与浏览器端到端测试。
- **设备固件工程 (`firmware/`)**：
  - `firmware/main/`：包含驱动、输入接收、协议转换与 BLE 通信。
  - `firmware/main/ui/ui_service.h`：固件核心与 UI 之间的状态快照装配与动作分发契约。
- **`firmware/main/` 模块**：
  - `bridge/`：控制面命令与事件分发。
  - `config/`：NVS 设置持久化。
  - `console/`：串口 CLI。
  - `netlog/`：局域网调试与 UDP 会话。
  - `dp/`：数据面任务与输入源调度。
  - `input/`：输入协议解析与串口/网络接收。
  - `usb/`：USB host 直插枚举、HID 收发与音频触觉。
  - `ota/`：固件升级会话管理。
  - `pad/`：手柄报告解析、归一化与布局映射。
  - `target/`：目标协议编码（NS2）。
  - `ble/`：BLE 外设服务与连接管理。
  - `drivers/`：屏幕、触摸、按键、蜂鸣器与电池驱动。
  - `ui/`：屏幕 UI 契约实现（状态快照装配与动作分发）。
  - 顶层 `main.c`：启动装配入口。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **屏幕 UI 宿主用例** | `cargo test --locked --manifest-path ui/Cargo.toml [用例名片段]` | 执行开发机 UI 宿主单元测试 |
| **屏幕 UI WASM 预览** | `cd ui/preview ; pnpm dev` | 启动浏览器端 WASM 界面预览（存盘自动重编并刷新） |
| **屏幕 UI 浏览器端到端用例** | `cd ui/preview ; pnpm test` | 执行浏览器端自动化端到端测试 |
| **屏幕 UI Rust 格式化与 Lint** | `cargo fmt --manifest-path ui/Cargo.toml --all` | 执行 UI 代码格式化与 Clippy 检查 |
| **固件主机端测试** | `uv run python scripts/firmware-test.py` | 运行固件纯逻辑主机端测试 |
| **PC 侧主机端测试** | `uv run python -m unittest discover -s pc/tests -t pc` | 运行 PC 工具纯逻辑单元测试 |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 设置芯片目标并生成配置 |
| **固件编译** | `cd firmware ; idf.py build` | 编译完整固件（-DREMAPAD_UI=OFF 为无 UI 构建） |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入监视器 |
| **固件增量烧录** | `cd firmware ; idf.py -p COMx app-flash` | 仅烧录应用分区 |
| **固件 C 格式化与静态检查** | `clang-format -i <改动的 .c/.h>` | 格式化 C 语言代码 |
| **固件 OTA 升级** | `uv run python pc/ctrl.py -p COMx --upgrade` | 执行固件 OTA 升级（支持串口与 -n 网络模式） |
| **PC 手柄桥接** | `uv run python pc/ctrl.py -p COMx` | 运行 PC 手柄桥接服务与 CLI 控制台 |
| **PC 连接控制台** | `uv run python pc/gui.py` | 启动 PC 图形管理控制台 |
| **MCP 按键服务** | `uv run python pc/mcp_server.py -p COMx` | 启动按键注入 MCP 服务供 agent 调用（`-n HOST[:PORT]` 走 WiFi） |
| **串口 CLI** | `uv run python pc/ctrl.py -p COMx status` | 执行单条设备 CLI 命令 |
| **局域网日志收听** | `uv run python scripts/netlog_listen.py [--port 9999]` | 监听设备 UDP 调试日志并发送命令 |
| **主机输出原始采集** | `uv run python pc/ctrl.py -p COMx --capture host-raw.log` | 抓取主机输出原始数据包 |
| **amiibo 镜像上传** | `uv run python pc/ctrl.py -p COMx --amiibo Alm.bin` | 上传 NTAG215 amiibo 镜像到设备 |
| **实机截图** | `uv run python pc/ctrl.py -p COMx --shot` | 截取设备屏幕当前画面为 PNG |

固件命令要在**配置本工程时用的那套 ESP-IDF 环境**里执行；配置用的解释器记录在 `firmware/build/CMakeCache.txt`
（`rg -n '^PYTHON' firmware/build/CMakeCache.txt`）。
切到另一套 IDF 环境时 `idf.py` 只打印环境提示就返回、不编译；判据与排错见 [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) 的「6. 编译 ESP-IDF 固件」。

## 产物与生成文件约定

- 禁止手动修改构建产物：`firmware/build/` 与 `ui/target/`（宿主用例的 cargo 产物）均由构建脚本全自动生成。
- 项目相关的临时文件（脚本、抓包与 `--dump` 输出、截图、日志、一次性分析产物）一律放 `agent-temp/`（`.gitignore` 已忽略其内容，只有 `.gitkeep` 入库）；
  要长期保留的东西再按各自目录约定落位。

## 项目特有约束

- **文档分层归位**：协议与设备数据进 [docs/controller-switch2.md](docs/controller-switch2.md) 与
  [docs/controller-ps.md](docs/controller-ps.md)；系统结构、链路与实现进 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 与
  [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md)；操作、命令与排错进本文件、[docs/GETTING-STARTED.md](docs/GETTING-STARTED.md)、
  [ui/README.md](ui/README.md)、[pc/README.md](pc/README.md) 与 [docs/TESTING.md](docs/TESTING.md)；决策与取舍只进 `docs/adr/`。
  同一份数据只在一处维护，其他位置写成一行引用。
- **流程一律画图**：流程、时序与状态机写成对应功能文档里的 mermaid 图（`flowchart` / `sequenceDiagram` / `stateDiagram-v2`），
  不在文档正文与代码注释里用文字复述步骤。
- **注释只写用途与边界**：文件头 3-6 行写职责与上下边界；公开函数一句用途，语义不自明的参数或返回值一行；
  契约级不变量、单位与线程/内存约束各一句。
- **每个文件最多一行文档指向**：需要引用时只写「数据与核对状态见 docs/controller-*.md」这类文件级指向，不逐字段引用。
  术语按本文「名词约定」，正文一句一行，单行不超过 120 字符。
- **提交信息以主题行为主**：主题行沿用 `<type>(<scope>): <subject>` 约定，一句话说清改了什么；
  正文只在改动意图从 diff 看不出来时补「为什么」，至多五行，不逐文件复述改动内容；
  验证结果（测试、编译与实机确认）、踩坑过程与待办事项不入提交信息。
- **单行不超过 120 字符**：`.editorconfig` 的 `max_line_length = 120` 适用于代码、脚本与文档；Markdown 正文按句子断行，一句一行，整句过长就改短。
  上游快照与锁文件（`Cargo.lock`）、单行 SVG 豁免；
  GFM 表格行（单元格不能折行）与必须整行粘贴执行的命令保持原样。

## 文档维护触发映射

修改文档时保持现有标题层级与内容顺序，新增内容就地追加或局部修改，避免无关的重排、重编号与大面积 diff。

| 变更范围 | 应同步维护的文档 |
| :--- | :--- |
| 硬件规格、屏幕驱动、Flash/PSRAM 配置变动 | [docs/hardware.md](docs/hardware.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) |
| 板卡外设、GPIO 分配、总线地址变动 | [docs/hardware.md](docs/hardware.md) |
| 产品定位、服务受众、非目标边界变动 | [docs/VISION.md](docs/VISION.md) |
| 跨层数据协议、核心图元、宏常量与状态模型变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 主机协议（广播/GATT/HID 报告/指令集/出厂块/NFC）变动 | [docs/controller-switch2.md](docs/controller-switch2.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 输入设备的字段偏移、输出报告、触觉通路或手柄行为设置变动 | [docs/controller-ps.md](docs/controller-ps.md), [docs/controller-xbox.md](docs/controller-xbox.md), [docs/controller-xinput.md](docs/controller-xinput.md), [docs/controller-ns1.md](docs/controller-ns1.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md) |
| 输入通路（桥接帧协议、私有格式、家族表、目标编码）变动 | [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md), [pc/README.md](pc/README.md) |
| 显示通路的行带提交/刷新取值、重画范围与动效代价或面板时钟变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 屏幕界面的布局、交互与字形烘焙规则变动 | [ui/README.md](ui/README.md), [docs/TESTING.md](docs/TESTING.md) |
| OTA 升级通路、桥接帧类型或载荷布局变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), [pc/README.md](pc/README.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| Python 工程结构、uv 依赖或命令入口变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), [pc/README.md](pc/README.md), 本文件 (`AGENTS.md`) |
| 测试入口、用例范围、回归规则或断言分层变动 | [docs/TESTING.md](docs/TESTING.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

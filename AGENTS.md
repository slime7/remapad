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

1. [产品愿景与边界 (docs/VISION.md)](docs/VISION.md)：项目定位、目标受众与非目标。
2. [系统架构与技术实现 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)：双工作区组成、数据流与构建流水线。
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：界面组件模型、字形烘焙与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：环境搭建、常用命令与调试排错。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：既定架构决策与选型取舍。
6. [Switch 2 手柄协议规范 (docs/controller-switch2.md)](docs/controller-switch2.md)：广播、GATT、HID 报告、配对、指令集、出厂块与 NFC。
7. [PS 家族手柄数据规范 (docs/controller-ps.md)](docs/controller-ps.md)：DS3 / DS4 / DualSense 的输入输出报告、触觉通路与行为设置；
   其余输入设备的规范是同目录下的 [controller-xbox.md](docs/controller-xbox.md)（Xbox 蓝牙报告与精英背键）、
   [controller-xinput.md](docs/controller-xinput.md)（XInput 形态的 Xbox 360 报文）与
   [controller-ns1.md](docs/controller-ns1.md)（Switch 一代的报文体与震动编码）。
8. [目标硬件参考 (docs/hardware.md)](docs/hardware.md)：SoC/存储、屏幕与触摸器件、外设地址、GPIO 分配与板级注意事项。
9. [测试策略与回归规则 (docs/TESTING.md)](docs/TESTING.md)：测试运行方式、断言分层与「先写用例再修 bug」规则。
10. [屏幕 UI 工作区 (ui/README.md)](ui/README.md)：界面源码与资源目录、构建链路、工具链与界面用例的运行方式。

## 项目工程架构与工作区划分

- **屏幕 UI 工程 (`ui/`)**：
  - 界面源码（声明式界面框架）在 `ui/src/`：根组件、页面与复用控件，目录表与构建链见 [ui/README.md](ui/README.md)。
  - `ui/` 是 Cargo workspace（单一 `Cargo.lock`）：`host/` 是宿主侧用例包，`slint_ui/` 是交叉编译进固件的界面组件，
    `render-plan/` 是平台层与宿主用例共用的行带计划，`build-support/` 是两份 build.rs 共用的编译口径（风格/字号/字体）；
    日常 `cargo test --manifest-path ui/Cargo.toml` 只跑 host。
  - 开发机上预览界面用 `uv run python scripts/ui-preview.py`：上半是 240 × 280 的设备画面（与固件同一棵 `AppContent`），
    下半是控制条，动作在预览里按固件语义结算，点着就能走一遍界面；改完存盘即刷新。
- **设备固件工程 (`firmware/`)**：
  - 基于 ESP-IDF `>=6.0,<6.2` 与 C 语言；硬件绑定微雪 ESP32-S3-Touch-LCD-1.69
    （16MB Flash + 8MB Octal PSRAM，240×280 ST7789V2 触摸屏），规格与引脚见 [docs/hardware.md](docs/hardware.md)。
  - 固件核心对界面知道的全部内容是 `firmware/main/ui/ui_service.h` 契约：状态快照装配、动作分发与生命周期入口；
    实现按构建形态选择——界面组件在 `ui/slint_ui` 与固件一起编译，
    或无 UI 构建的空实现；
    改动调度前先读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。
  - 产品固件负责 USB 接收、NS2 报告转换、BLE 广播/GATT/配对和显示提交。
- **`firmware/main/` 模块**：
  - `bridge/`：控制面命令/事件，PWR 按键与串口 CLI 经外部队列汇入。
  - `config/`：NVS 用户设置持久化；setter 只置内存表脏标记，提交任务每 1 分钟检查一次，确有改动才写一次 NVS。
  - `console/`：串口 CLI 与控制台出口切换；切到 USB host 后日志与 CLI 走 UART0。
  - `dp/`：数据面任务、输入源抽象与组合键捕获屏幕。
  - `input/`：输入通路接收段：桥接帧协议、USB-Serial/JTAG 唯一读取者、桥接输入源。
  - `usb/`：USB host 直插：枚举与 HID 收发、输入源、运行时角色切换；DualSense 的音频触觉通道也挂在这一层
    （`usb_audio.c` 自写最小 UAC1 等时客户端、`haptic_synth.c` 板上合成 PCM）。
  - `ota/`：升级会话：非运行分区回写、窗口流控与回滚健康门槛。
  - `pad/`：处理段：私有格式 `pad_state_t`、解析与归一、按布局行编码的反馈；
    家族布局表按系列拆在 `pad/layouts/`，契约与注册表是 `pad/layout.h` / `pad/layout.c`；
    DS4 / DS5 的触摸板按键行为在 `pad/ds_behavior.h` / `pad/ds_behavior.c`。
  - `target/`：转换段：目标编码接口 `pad_target_t`；`target/ns2/` 负责 NS2 编码、序列号命名规则与输出封装。
  - `ble/`：NimBLE 手柄外设、双身份会话与分槽凭证。
  - `drivers/`：panel / touch / backlight / pwr_key / buzzer / battery；
    面板刷新取值与行带提交见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 的「内存与显示策略」。
  - `ui/`：屏幕 UI 契约的 core 侧实现——状态快照装配与动作分发（无 UI 构建另有空实现 stub）。
  - 顶层 `main.c`：启动装配（控制面服务任务与界面提供者都由它拉起）。
  - PC 侧程序在 `pc/`（`remapadctl.py`：转发 + 命令行 + 实机截图 + OTA + DS5 音频触觉合成 `ds5_haptics.py`；
    `remapadgui.py`：同一套会话的图形界面），见 [pc/README.md](pc/README.md)。
  - 新增输入设备按 `dp/dp_source.h` 的输入源接口注册，不要绕过它直连编码器。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **屏幕 UI 预览** | `uv run python scripts/ui-preview.py` | 打开 `ui/preview.slint`：设备画面 240 × 280 在上、控制条在下，动作在预览里结算，改完存盘即刷新；`--file ui/src/app.slint` 只看设备画面，`--check` 只编译打印诊断，`--screenshot <png>` 渲染一帧存图（预览工具的装法见 [ui/README.md](ui/README.md)） |
| **屏幕 UI 宿主用例** | `cargo test --locked --manifest-path ui/Cargo.toml [用例名片段]` | 在开发机上编译真实界面产物（界面测试后端 + 软件渲染器），按元素几何与像素断言屏幕行为；改界面先加一条能复现的红用例，其余用例等改完再整跑 |
| **固件主机端测试** | `uv run python scripts/firmware-test.py` | 把与硬件无关的固件逻辑编译成开发机可执行文件并运行，秒级出结果 |
| **PC 侧主机端测试** | `cd pc ; uv run python -m unittest discover -s tests -t .` | `pc/` 工具里与设备无关的纯逻辑（串口枚举、镜像校验、帧编解码、输出分流、工具命令解析）在 `pc/tests/` 用标准库 unittest 跑，不接设备 |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件（默认带屏幕 UI，需要 xtensa Rust 工具链）；`idf.py -DREMAPAD_UI=OFF build` 走纯 C 的无 UI 构建（不需要 Rust，屏幕熄灭、设置走串口 CLI） |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器；禁止对已写入用户数据的设备执行 `erase-flash`（会清空 NVS 设置/配对与 `storage` 分区） |
| **固件增量烧录** | `cd firmware ; idf.py -p COMx app-flash` | 仅重写应用分区（`ota_0` @ 0x10000）；改动 bootloader/分区表后仍需完整烧录 |
| **固件 OTA 升级** | `cd pc ; uv run python remapadctl.py -p COMx --upgrade` | 经 USB-Serial/JTAG 推送 `firmware/build/remapad_firmware.bin`（界面已编进应用）到非运行分区，校验通过后自动重启；`--dry-run` 只校验镜像、`--wait` 等设备回来后打印版本；从 `ota_1` 启动后继续开发要先 `idf.py erase-otadata` |
| **PC 手柄桥接** | `cd pc ; uv run python remapadctl.py -p COMx` | 读 PC 手柄原始报告按桥接帧转发给设备，同进程提供串口命令行、实机截图与 OTA；`--list` 枚举手柄、`--dump` 抓原始报告核对家族表偏移；转发默认只在交互模式开，`--pad` / `--no-pad` 控制 |
| **PC 连接控制台** | `cd pc ; uv run python remapadgui.py` | 同一套会话的图形界面：选串口、连接/断开、手柄转发开关、实时日志、命令输入、屏幕设置（亮度、手柄配色、DS4/DS5、电源）、实机截图与 OTA；调试动作只在「命令」页；与命令行不要同时连同一个口 |
| **串口 CLI** | `cd pc ; uv run python remapadctl.py -p COMx status` | 行命令控制台：位置参数透传设备命令、`--log` 只读日志、交互模式 `:help` 看工具命令；常用设备命令有 `link`、`headset`、`shot`、`key ui` 与 `ui on\|off`、`capture on\|off`、`ds touchpad\|capture on\|off`、`amiibo list\|select\|del\|poll`、`version`、`rollback`；屏幕重绘诊断用 `trace [frames]`（每帧一行渲染耗时、提交耗时、damage 像素数与矩形条数） |
| **主机输出原始采集** | `cd pc ; uv run python remapadctl.py -p COMx --capture host-raw.log` | 抓主机写进输出特征值的原始字节（震动/玩家灯/指令，解析与布局转换之前）落盘成文本；桥接帧 `0x12`（HOST_RAW）承载，串口 `capture on\|off` 开关，交互模式 `:capture <路径>\|off` 同能力，`--pad` 可与手柄转发同时进行 |
| **amiibo 镜像上传** | `cd pc ; uv run python remapadctl.py -p COMx --amiibo Alm.bin` | 经桥接帧（`0x40-0x43`）把 NTAG215 dump（540 纯镜像或 572 带厂商签名）传进设备 storage 分区 SPIFFS 槽位（200 槽，槽位名取文件名主干）；交互模式 `:amiibo <bin>` 同通道，选中持久化、重启恢复 |
| **实机截图** | `cd pc ; uv run python remapadctl.py -p COMx --shot` | 固件把当前画面整屏重渲染并按图像帧回传，PC 拼成 PNG（默认 `pc/shots/`，`--out` 指定路径；期间 UI 冻结约 0.2-1 秒） |

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
| 测试入口、用例范围、回归规则或断言分层变动 | [docs/TESTING.md](docs/TESTING.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

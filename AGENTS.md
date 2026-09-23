# Remapad Agent 开发与维护指南

Remapad 是面向搭载屏幕的微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8）的嵌入式控制器系统：
USB 输入 → NS2 手柄报告 → BLE 手柄，配套 Slint 屏幕 UI。
工程分为 `ui/`（Slint 屏幕 UI 与宿主用例）与 `firmware/`（ESP-IDF 固件）两个工作区；
主机协议资料见 [docs/controller-switch2.md](docs/controller-switch2.md)，输入设备数据见 [docs/controller-ps.md](docs/controller-ps.md)，
板卡规格见 [docs/hardware.md](docs/hardware.md)。

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
3. [核心概念与领域抽象 (docs/ABSTRACTIONS.md)](docs/ABSTRACTIONS.md)：Slint 组件模型、字形烘焙与软硬件契约。
4. [新手开发与上手指南 (docs/GETTING-STARTED.md)](docs/GETTING-STARTED.md)：环境搭建、常用命令与调试排错。
5. [架构决策记录索引 (docs/adr/README.md)](docs/adr/README.md)：既定架构决策与选型取舍。
6. [Switch 2 手柄协议规范 (docs/controller-switch2.md)](docs/controller-switch2.md)：广播、GATT、HID 报告、配对、指令集、出厂块与 NFC。
7. [PS 家族手柄数据规范 (docs/controller-ps.md)](docs/controller-ps.md)：DS3 / DS4 / DualSense 的输入输出报告、触觉通路与行为设置；
   其余输入设备的规范是同目录下的 [controller-xbox.md](docs/controller-xbox.md)（Xbox 蓝牙报告与精英背键）、
   [controller-xinput.md](docs/controller-xinput.md)（XInput 形态的 Xbox 360 报文）与
   [controller-ns1.md](docs/controller-ns1.md)（Switch 一代的报文体与震动编码）。
8. [目标硬件参考 (docs/hardware.md)](docs/hardware.md)：SoC/存储、屏幕与触摸器件、外设地址、GPIO 分配与板级注意事项。
9. [测试策略与回归规则 (docs/TESTING.md)](docs/TESTING.md)：测试运行方式、断言分层与「先写用例再修 bug」规则。

## 项目工程架构与工作区划分

- **屏幕 UI 工程 (`ui/`)**：
  - 界面是 Slint（`.slint`）：`ui/src/` 下是根组件、页面与复用控件，目录表与构建链见 [ui/README.md](ui/README.md)。
  - `ui/Cargo.toml` 是宿主侧包：`build.rs` 用与固件组件同一套口径编译 `src/app.slint`，
    `ui/tests/*.rs` 是界面用例（Slint 测试后端 + 软件渲染器，`cargo test --manifest-path ui/Cargo.toml`）。
  - 开发机上预览界面用 `uv run python scripts/ui-preview.py`：上半是 240 × 280 的设备画面（与固件同一棵 `AppContent`），
    下半是控制条，动作在预览里按固件语义结算，点着就能走一遍界面；改完存盘即刷新。
- **设备固件工程 (`firmware/`)**：
  - 基于 ESP-IDF `>=6.0,<6.2` 与 C 语言；硬件绑定微雪 ESP32-S3-Touch-LCD-1.69
    （16MB Flash + 8MB Octal PSRAM，240×280 ST7789V2 触摸屏），规格与引脚见 [docs/hardware.md](docs/hardware.md)。
  - 屏幕状态由 `firmware/main/slint_host.c` 每轮轮询写进界面、动作经回调交回；界面在 `firmware/components/slint_ui` 里
    与固件一起编译（Rust，[ADR 0054](docs/adr/0054-screen-ui-slint-rust.md)），改动调度前先读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。
  - 产品固件负责 USB 接收、NS2 报告转换、BLE 广播/GATT/配对和显示提交。
- **`firmware/main/` 模块**：
  - `bridge/`：控制面命令/事件，PWR 按键与串口 CLI 经外部队列汇入。
  - `config/`：NVS 用户设置持久化；setter 只置内存表脏标记，提交任务每 1 分钟检查一次，确有改动才写一次 NVS。
  - `console/`：串口 CLI 与控制台出口切换；切到 USB host 后日志与 CLI 走 UART0。
  - `dp/`：数据面任务、输入源抽象与组合键捕获屏幕（`dp_ui.c`，[ADR 0028](docs/adr/0028-pad-combo-captures-screen.md)）；
    数据面汇合约定见 [ADR 0011](docs/adr/0011-controller-dataplane-module-boundary.md)。
  - `input/`：输入通路接收段：桥接帧协议、USB-Serial/JTAG 唯一读取者、桥接输入源。
    三段边界见 [ADR 0021](docs/adr/0021-input-path-three-stage-layering.md)，
    同代透传规则见 [ADR 0026](docs/adr/0026-same-generation-input-passthrough.md)。
  - `usb/`：USB host 直插：枚举与 HID 收发、输入源、运行时角色切换；DualSense 的音频触觉通道也挂在这一层
    （`usb_audio.c` 自写最小 UAC1 等时客户端、`haptic_synth.c` 板上合成 PCM），
    取舍见 [ADR 0042](docs/adr/0042-ds5-audio-haptics-onboard-synthesis.md)。
    运行时角色切换见 [ADR 0027](docs/adr/0027-runtime-usb-role-switch.md)。
  - `ota/`：升级会话：非运行分区回写、窗口流控与回滚健康门槛（[ADR 0022](docs/adr/0022-ota-over-bridge-frames-with-rollback.md)）。
  - `pad/`：处理段：私有格式 `pad_state_t`、解析与归一、按布局行编码的反馈；
    家族布局表按系列拆在 `pad/layouts/`，契约与注册表是 `pad/layout.h` / `pad/layout.c`；
    DS4 / DS5 的触摸板按键行为在 `pad/ds_behavior.h` / `pad/ds_behavior.c`（[ADR 0047](docs/adr/0047-ds-behavior-settings.md)）。
  - `target/`：转换段：目标编码接口 `pad_target_t`；`target/ns2/` 负责 NS2 编码、序列号命名规则与输出封装。
  - `ble/`：NimBLE 手柄外设、双身份会话与分槽凭证。
  - `drivers/`：panel / touch / backlight / pwr_key / buzzer / battery；
    面板刷新取值与行带提交见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 的「内存与显示策略」。
  - 顶层 `boot_splash.c`：UI 就绪前的启动画面，随面板启动点亮背光；`slint_host.c`：屏幕状态装配与动作分发。
  - PC 侧程序在 `pc/`（`remapadctl.py`：转发 + 命令行 + 实机截图 + OTA + DS5 音频触觉合成 `ds5_haptics.py`；
    `remapadgui.py`：同一套会话的图形界面），见 [pc/README.md](pc/README.md)。
  - 新增输入设备按 `dp/dp_source.h` 的输入源接口注册，不要绕过它直连编码器。

## 项目核心操作命令

| 操作项 | 执行指令 | 说明 |
| :--- | :--- | :--- |
| **屏幕 UI 预览** | `uv run python scripts/ui-preview.py` | 用 slint-viewer 打开 `ui/preview.slint`：设备画面 240 × 280 在上、控制条在下，动作在预览里结算，改完存盘即刷新；`--file ui/src/app.slint` 只看设备画面，`--check` 只编译打印诊断，`--screenshot <png>` 渲染一帧存图 |
| **屏幕 UI 宿主用例** | `cargo test --locked --manifest-path ui/Cargo.toml [用例名片段]` | 在开发机上编译真实 `.slint` 产物（Slint 测试后端 + 软件渲染器），按元素几何与像素断言屏幕行为；改界面先加一条能复现的红用例，其余用例等改完再整跑 |
| **固件主机端测试** | `uv run python scripts/firmware-test.py` | 把与硬件无关的固件逻辑编译成开发机可执行文件并运行，秒级出结果 |
| **PC 侧主机端测试** | `cd pc ; uv run python -m unittest discover -s tests -t .` | `pc/` 工具里与设备无关的纯逻辑（串口枚举、镜像校验、帧编解码、输出分流、工具命令解析）在 `pc/tests/` 用标准库 unittest 跑，不接设备 |
| **固件配置** | `cd firmware ; idf.py set-target esp32s3` | 配置目标芯片架构并合并硬件预设 |
| **固件编译** | `cd firmware ; idf.py build` | 编译 ESP-IDF 完整固件 |
| **固件烧录** | `cd firmware ; idf.py -p COMx flash monitor` | 烧录固件并进入串口监视器；禁止对已写入用户数据的设备执行 `erase-flash`（会清空 NVS 设置/配对与 `storage` 分区，见 [ADR 0009](docs/adr/0009-ota-storage-flash-layout.md)） |
| **固件增量烧录** | `cd firmware ; idf.py -p COMx app-flash` | 仅重写应用分区（`ota_0` @ 0x10000）；改动 bootloader/分区表后仍需完整烧录 |
| **固件 OTA 升级** | `cd pc ; uv run python remapadctl.py -p COMx --upgrade` | 经 USB-Serial/JTAG 推送 `firmware/build/remapad_firmware.bin`（界面已编进应用）到非运行分区，校验通过后自动重启；`--dry-run` 只校验镜像、`--wait` 等设备回来后打印版本；从 `ota_1` 启动后继续开发要先 `idf.py erase-otadata` |
| **PC 手柄桥接** | `cd pc ; uv run python remapadctl.py -p COMx` | 读 PC 手柄原始报告按桥接帧转发给设备，同进程提供串口命令行、实机截图与 OTA；`--list` 枚举手柄、`--dump` 抓原始报告核对家族表偏移；转发默认只在交互模式开，`--pad` / `--no-pad` 控制 |
| **PC 连接控制台** | `cd pc ; uv run python remapadgui.py` | 同一套会话的图形界面：选串口、连接/断开、手柄转发开关、实时日志、命令输入、屏幕设置（亮度、手柄配色、DS4/DS5、电源）、实机截图与 OTA；调试动作只在「命令」页；与命令行不要同时连同一个口 |
| **串口 CLI** | `cd pc ; uv run python remapadctl.py -p COMx status` | 行命令控制台：位置参数透传设备命令、`--log` 只读日志、交互模式 `:help` 看工具命令；常用设备命令有 `link`、`headset`、`shot`、`key ui` 与 `ui on\|off`、`capture on\|off`、`ds touchpad\|capture on\|off`、`amiibo list\|select\|del\|poll`、`version`、`rollback`；屏幕重绘诊断用 `trace [frames]`（每帧一行 damage 计划与逐条行带耗时）与 `drawlist`（把本帧绘制指令按十六进制字转储，供 PC 侧离线解码） |
| **主机输出原始采集** | `cd pc ; uv run python remapadctl.py -p COMx --capture host-raw.log` | 抓主机写进输出特征值的原始字节（震动/玩家灯/指令，解析与布局转换之前）落盘成文本；桥接帧 `0x12`（HOST_RAW）承载，串口 `capture on\|off` 开关，交互模式 `:capture <路径>\|off` 同能力，`--pad` 可与手柄转发同时进行，见 [ADR 0045](docs/adr/0045-host-output-raw-capture.md) |
| **amiibo 镜像上传** | `cd pc ; uv run python remapadctl.py -p COMx --amiibo Alm.bin` | 经桥接帧（`0x40-0x43`）把 NTAG215 dump（540 纯镜像或 572 带厂商签名）传进设备 storage 分区 SPIFFS 槽位（200 槽，槽位名取文件名主干）；交互模式 `:amiibo <bin>` 同通道，选中持久化、重启恢复，标签模拟见 [ADR 0044](docs/adr/0044-amiibo-bridge-upload-nfc-tag-emulation.md) |
| **实机截图** | `cd pc ; uv run python remapadctl.py -p COMx --shot` | 固件把当前画面整屏重渲染并按图像帧回传，PC 拼成 PNG（默认 `pc/shots/`，`--out` 指定路径；期间 UI 冻结约 0.2-1 秒，见 [ADR 0033](docs/adr/0033-pc-single-process-tool-and-device-screenshot.md)） |

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
  [pc/README.md](pc/README.md) 与 [docs/TESTING.md](docs/TESTING.md)；决策与取舍只进 `docs/adr/`。
  同一份数据只在一处维护，其他位置写成一行引用。
- **流程一律画图**：流程、时序与状态机写成对应功能文档里的 mermaid 图（`flowchart` / `sequenceDiagram` / `stateDiagram-v2`），
  不在文档正文与代码注释里用文字复述步骤。
- **注释只写用途与边界**：文件头 3-6 行写职责与上下边界；公开函数一句用途，语义不自明的参数或返回值一行；
  契约级不变量、单位与线程/内存约束各一句。删除流程叙述、实测数据、抓包记录、踩坑过程与逐处文档章节引用。
- **每个文件最多一行文档指向**：需要引用时只写「数据与核对状态见 docs/controller-*.md」这类文件级指向，不逐字段引用。
  术语按本文「名词约定」，正文一句一行，单行不超过 120 字符。
- **提交信息以主题行为主**：主题行沿用 `<type>(<scope>): <subject>` 约定，一句话说清改了什么；
  正文只在改动意图从 diff 看不出来时补「为什么」，至多五行，不逐文件复述改动内容；
  验证结果（测试、编译与实机确认）、踩坑过程与待办事项不入提交信息。

- **连接由用户发起**：上电静默，只有连接键（配对页「连接」、PWR 长按 3 秒）打开连接窗口；
  主机主动断开后自动开 30 秒回连窗口（只发回连形态、不带唤醒突发），到期静默；
  未连接时按手柄 HOME（实体手柄按下去或调试页注入）打开唤醒窗口把休眠主机叫起来；窗口到期或主机连上即收窗。
  改这条策略前先读 [ADR 0038](docs/adr/0038-user-initiated-connection-window.md)。
- **先写测试再改代码**：改屏幕 UI 的 bug 先在 `ui/tests/` 加一条能复现的红用例（宿主侧跑真实产物），改完让用例转绿才算修完；
  固件里与硬件无关的逻辑缺陷先补 `firmware/test/` 的主机端用例，PC 侧工具同理先补 `pc/tests/` 的用例。任何业务改动都一样：
  没红过不许改代码，没转绿不算改完。用例标题写用户看到的现象，不放宽断言迁就实现，同一行为只留一条用例。
- **优先端到端测试**：能在端到端层断言的行为（屏幕画面与交互、真实产物链路）就在端到端层写；
  单元用例只补端到端覆盖不到的纯逻辑。
- **开发期间不跑端到端套件**：迭代中只跑当前这条用例，端到端套件等全部改完再整跑一次；
  各套用例的位置与运行方式见 [docs/TESTING.md](docs/TESTING.md)。
- **测试只用真源码**：屏幕 UI 用例编译 `ui/src/*.slint` 真实产物并在软件渲染器上渲染画面，固件测试编译 `firmware/main/` 下的源码；
  `firmware/test/support/stubs/` 只补齐主机缺失的 ESP-IDF 头文件与硬件取值入口，不得把被测逻辑复制一份进测试。
- **手柄操控屏幕模式**：手柄按 L1+R1+L3+R3（按住 300 ms）捕获输入，此后只向主机续发全松开的中性帧、玩家按键不再上行（整段停发会被主机判离线），
  改为方向键翻页与移动焦点（焦点在可聚焦项之间循环，到底再按回到另一端）、圆圈键确认（[ADR 0028](docs/adr/0028-pad-combo-captures-screen.md)）；
  PC 预览的「手柄操控」键能把焦点环点亮走查（动作在预览里结算），真机上用 `key ui` 或 `ui on\|off` 进出。
  新增可点控件要带焦点环（各页按 `focused` 画 2px 环，取 theme.slint 的 focus-ring），
  并且只在 App 传下来的 `interactive` 为真时进焦点名单，否则隐藏页的可点元素会留在名单里；
  新增可聚焦行时同步 [ui/src/app.slint](ui/src/app.slint) 的 `focus-count-for`（固件按这个行数走焦点）。
- **重绘成本按像素算**：每帧的重绘价格等于这帧碰了多少像素，动效与控件写法按 [ADR 0050](docs/adr/0050-repaint-friendly-screen-rules.md) 选。
  圆角 + 边框的元素必须同时给底色（取所在面的颜色）：只描边框会退化成逐行覆盖条，重画范围按整个包围盒算。
  翻页时卡片从行进侧滑入 16px（90 ms）并让同侧箭头弹一下：这段位移每帧重画整个内容框，时长不要再加长；
  只有拖动预览保留跟手平移。
  卡片与底栏底图是构建期光栅化的位图（`ui/assets/*.svg`），改底图底色要同步 `theme.slint` 的 `Theme.background`（窗口底色）。
  量重画范围与代价用串口 `trace [frames]`（逐帧渲染耗时、提交耗时、damage 像素数与矩形条数）。
- **字体与字形烘焙规则**：
  - 文本字号只取 theme.slint 里烘过的档位（12 / 14 / 16 / 24，与固件组件 build.rs 的 `FONT_SIZES` 一致）。
  - 构建期按 `.slint` 字面量自动子集烘字形位图：只出现在运行期拼出来的字符串里的码点不会被烘，
    必须写进 [ui/src/app.slint](ui/src/app.slint) 的字符集锚点串，否则上屏是空洞或豆腐块。
  - 单色图标用 Material Symbols 字形（`Icon { glyph: "\u{e30c}"; size: ...; tint: ...; }`），转圈用覆盖 U+28xx 的 seguisym；
    SVG 只留给卡片与底栏底图。
- **屏幕 UI 的构建与工具链**：
  - `firmware/components/slint_ui` 用 slint-build 在构建期编译 `ui/src/*.slint`，再交叉编译成静态库链进固件，
    需要 Espressif 的 xtensa Rust 工具链：换机步骤见 [ui/README.md](ui/README.md)，一键安装是 [scripts/setup-rust-toolchain.py](scripts/setup-rust-toolchain.py)。
  - 宿主用例用开发机的 stable 工具链跑；界面 id 是用例的查询入口，改 id 要同步 [ui/tests/](ui/tests)。
  - PocketJS 时代的组件、脚本与宿主（`firmware/components/pocketjs_*`、`patches/`、`.pocket` 包链）已随 [ADR 0054](docs/adr/0054-screen-ui-slint-rust.md) 整体移除。
- **工具链只用 Python 与 Rust**：仓库不含 Node.js / Bun，脚本一律是 `scripts/*.py` 与 `pc/` 下的 Python 源码，
  依赖与解释器都交给 uv：根目录 `pyproject.toml` + `uv.lock` 管 `scripts/`，`pc/pyproject.toml` + `pc/uv.lock` 管 PC 工具，
  统一用 `uv run python <路径>` 执行（`uv.lock` 要提交）。
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
| 显示通路的行带提交/刷新取值、重画范围与动效代价或面板时钟变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/adr/0018](docs/adr/0018-panel-spi2-clock-80mhz.md), [docs/adr/0050](docs/adr/0050-repaint-friendly-screen-rules.md), [docs/adr/0054](docs/adr/0054-screen-ui-slint-rust.md) |
| 屏幕界面的布局、交互与字形烘焙规则变动 | [ui/README.md](ui/README.md), [docs/TESTING.md](docs/TESTING.md), [docs/adr/0054](docs/adr/0054-screen-ui-slint-rust.md) |
| OTA 升级通路、桥接帧类型或载荷布局变动 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/ABSTRACTIONS.md](docs/ABSTRACTIONS.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), [pc/README.md](pc/README.md), [docs/adr/0022](docs/adr/0022-ota-over-bridge-frames-with-rollback.md) |
| 环境依赖、操作指令、目录结构变动 | [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 测试入口、用例范围、回归规则或断言分层变动 | [docs/TESTING.md](docs/TESTING.md), [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md), 本文件 (`AGENTS.md`) |
| 产生新的长期架构决策与技术选型取舍 | 使用 [scripts/create_adr.py](scripts/create_adr.py) 新建 ADR 并更新 [docs/adr/README.md](docs/adr/README.md) |

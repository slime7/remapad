# Remapad 新手开发与上手指南

本指南面向微雪 ESP32-S3-Touch-LCD-1.69 目标板，说明界面预览与用例、ESP-IDF 编译和当前 bring-up 边界。
Remapad 的最终产品链路是 USB 输入→NS2 手柄报告→BLE 输出，并通过屏幕 UI 管理连接和配对；
协议资料见 [controller-switch2.md](controller-switch2.md) 与 [controller-ps.md](controller-ps.md)，
板卡规格与引脚见 [hardware.md](hardware.md)。

## 前置环境

| 工具 | 版本/要求 | 用途 |
| :--- | :--- | :--- |
| Rust（宿主 stable） | 当前 stable | 编译界面与宿主用例（`cargo test`），以及安装预览工具 |
| Xtensa Rust | `esp-rs/rust-build` 的 `v1.97.0.0`（rustup 工具链名默认 `esp`） | 把 `ui/slint_ui` 交叉编译成 ESP32-S3 静态库；一键安装是 `uv run python scripts/setup-rust-toolchain.py` |
| slint-viewer | 1.18.1 | PC 预览界面的工具（装法与版本口径见 [ui/README.md](../ui/README.md)） |
| Python | 3.10 或更高（由 uv 准备；`idf.py` 另用 ESP-IDF 自带的解释器） | `scripts/*.py` 全部脚本与 `pc/` 下的工具 |
| uv | 当前稳定版 | 所有 Python 入口都经它执行：`uv run python scripts/<名字>.py`（根目录 `pyproject.toml` + `uv.lock`）与 `cd pc ; uv run python remapadctl.py -p COMx`（`pc/pyproject.toml` + `pc/uv.lock`，依赖是 `hidapi` 与 `customtkinter`） |
| ESP-IDF | `>=6.0,<6.2` | 本仓库已在 6.1 上验证 |
| 硬件 | 微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8） | 16 MB Flash、8 MB Octal PSRAM、240 × 280 ST7789V2 触摸屏；细节见 [hardware.md](hardware.md) |

目标 NS2 手柄型号和 BLE 天线/射频属于最终硬件范围。
两条输入路径（PC 桥接见 [pc/README.md](../pc/README.md)，手柄插板卡的 USB host 直插见「USB 手柄直插」一节）的代码都已落地，实机核对待做（VBUS 供电路径已按 V2.1 原理图确认为 TP1 外部注入）。
不要因为 PC 预览能看界面就认为真实 BLE 链路已经可用：预览只画排版，动作要连到固件才生效。

板卡已知信息都记录在 [hardware.md](hardware.md)：
屏幕为 ST7789V2（240 × 280，4-wire SPI），触摸为 CST816T（I2C `0x15`），面板和触摸的具体引脚、共享 I2C 总线、背光控制脚和 USB 口约束都在那里。
固件已通过 `drivers/` 中的 panel/touch/backlight BSP 点亮屏幕并上报触点；
BLE 手柄数据面已接入。
主机互操作已实机验证；其余板载外设没有接入计划（电池电压采样已接入，充电状态只能按电压趋势推断，见 [hardware.md](hardware.md)）。

## 最短步骤

### 1. 准备 Rust 工具链

```powershell
uv run python scripts/setup-rust-toolchain.py          # 装 Xtensa 工具链（缺什么装什么）
uv run python scripts/setup-rust-toolchain.py --check  # 只检查，不改动环境
```

PC 预览工具的安装命令见 [ui/README.md](../ui/README.md)。
界面与宿主用例只需要宿主 stable 工具链；Xtensa 工具链只服务于固件构建（`REMAPAD_UI=OFF` 的纯 C 构建不需要它），
换机步骤与 CMake 变量见 [ui/README.md](../ui/README.md)。
界面依赖由 `ui/Cargo.lock` 锁定，首次构建从 crates.io 拉取，之后走本地缓存。

仓库里的 Python 脚本都由 uv 托管：根目录 `pyproject.toml` + `uv.lock` 管 `scripts/`，`pc/` 自己一套（两份 `uv.lock` 都要入库）；
装好 uv 后 `uv run python <路径>` 会自动准备解释器与依赖，不需要手动建虚拟环境（首次执行会在仓库根建 `.venv`）。

### 2. 预览界面与跑宿主用例

```powershell
uv run python scripts/ui-preview.py                     # 交互预览：设备画面 240 × 280 + 控制条，存盘即刷新
uv run python scripts/ui-preview.py --file src/app.slint  # 只看设备画面
uv run python scripts/ui-preview.py --check              # 只编译并打印诊断
cargo test --locked --manifest-path ui/Cargo.toml          # 界面宿主用例（元素几何 + 画面像素）
```

预览按 1:1 逻辑像素打开（`SLINT_SCALE_FACTOR=1`），字体与字号表按固件构建同款口径喂给编译器，
因此中文与图标与实机同源；想放大看细节自己设 `SLINT_SCALE_FACTOR`。
预览窗的下半截是控制条：设备画面里的控件照常发动作，动作在预览里按固件语义结算，
所以翻页、拖动、亮度、确认弹窗、配对档位、USB 角色与 OTA 进度都能点着走一遍；
控制条还能直接点出手柄操控窗口的焦点环（「手柄操控 / 焦点 ± / 确认键」）。
预览只验界面与动作结算：电池、内存、版本号是模拟值，固件行为要在实机上验（串口 `key ui` / `ui on|off`）。

### 3. 检查界面改动

界面没有单独的 lint 或打包步骤：界面源码在固件构建期编译，语法与烘焙错误会直接出现在 `idf.py build` 的输出里。
只想快速验证语法用 `uv run python scripts/ui-preview.py --check`。

### 4. 编译 ESP-IDF 固件

固件命令要在**配置本工程时用的那套 ESP-IDF 环境**里执行：
`firmware/build/CMakeCache.txt` 记录了解释器路径（`rg -n '^PYTHON' firmware/build/CMakeCache.txt` 可以查到）。
换一套环境后 `idf.py` 会空转、不编译（见「常见问题」）。

```powershell
. <IDF_DIR>\export.ps1
cd firmware
idf.py set-target esp32s3
idf.py build
```

`<IDF_DIR>` 换成自己的 ESP-IDF 安装目录：PowerShell 用该目录下的 `export.ps1`，bash 用 `export.sh`；
用官方安装管理器装的 IDF，就用它生成的 profile 脚本或 IDE 终端。同一台机器上可能并存多套 IDF 工具环境，激活脚本与 Python 解释器都不同，只有配置工程时用的那套能直接构建。判断标准与机器无关：

- 真的开始编译（出现 `[1/N] Building ...`）才算成功；
- 只打印几行环境提示、随后一行 `Executing action: all (aliases: build)` 就结束（退出码 0、`firmware/build/` 里的产物时间戳不变）是空转：
  输出里会同时给出「当前环境用的解释器」与「配置工程时用的解释器」两条路径，照着切回去即可；
- 想换到另一套环境长期使用，就在那套环境里 `idf.py fullclean` 后重新配置（代价是完整重编一次）。

`ui/slint_ui` 是放在 `ui/` 工作区里的 ESP-IDF 组件（`REMAPAD_UI` 开关默认 ON，经 `EXTRA_COMPONENT_DIRS` 引入），
它在配置阶段检查 cargo 与 Xtensa 工具链、在构建阶段用 cargo 把界面编成静态库；
`ui/src/*.slint` 与 `ui/assets/*.svg` 登记为构建依赖，改完界面直接 `idf.py build` 即可，不需要删除 `firmware/build/`。
带调试页的开发构建是默认值；发布构建设 `$env:REMAPAD_RELEASE = "1"` 后重跑配置。
不需要屏幕时 `idf.py -DREMAPAD_UI=OFF build` 走纯 C 构建：不引入界面组件、不需要 Rust 工具链，
面板/触摸/背光不初始化，设置经 PC 串口 CLI 控制。
组件构建的细节（工具链名、字体变量、字号表）见 [ui/README.md](../ui/README.md) 与 [ARCHITECTURE.md](ARCHITECTURE.md) 的「构建链路」。

### 5. 烧录与监视

```powershell
idf.py -p COM3 flash monitor
```

没有 ESP-IDF 终端时（例如从 Git Bash 直接发起），可以用一条 PowerShell 命令激活环境后执行：
用配置工程时那套 IDF 的 `export.ps1`（见上一节），先把 `MSYSTEM` 清掉——Git Bash 会把它带给子进程，`idf.py` 检测到后只打印警告并静默拒绝执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; . '<IDF_DIR>\export.ps1'; Set-Location firmware; idf.py -p COM3 flash"
```

`<IDF_DIR>` 与 `COM3` 按实际填写。

把 `COM3` 替换为实际端口。若开发板没有自动进入下载模式，按板卡说明操作 BOOT/EN。串口监视器使用 `Ctrl + ]` 退出。

`idf.py build` 在 `firmware/build/` 下生成三个可烧录文件，偏移与 `firmware/build/flash_project_args` 一致：
| 文件 | 烧录偏移 |
| :--- | :--- |
| `build/bootloader/bootloader.bin` | `0x0` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/remapad_firmware.bin` | `0x10000` |

界面已经编在应用镜像里，烧完这三个文件就是完整的设备固件。

日常迭代只改应用层（`ui/` 产物或 `firmware/main/`）时，bootloader 和分区表没有变化，可以只重写 `0x10000` 处的应用分区，比整片烧录快，对 Flash 的擦写也更少：

```powershell
idf.py -p COM3 app-flash monitor
```

改动 bootloader、分区表或 `sdkconfig` 后仍需完整 `flash`。esptool 的等价操作是对 `0x10000` 单独 `write-flash`。

分区表为终局布局（`ota_0`/`ota_1` 双应用分区 + `storage` 通用存储区，`ota_0` 继承原 factory 的 `0x10000`）。烧录时注意：

- 分区表布局变更后烧录分区表即可让已部署固件原地迁移为 `ota_0`，但保险起见直接完整 `flash`。
- `erase-flash` 会全片擦除，清掉 NVS 里的设置与 BLE 配对、`storage` 里的用户数据；设备交到用户手上之后不要再随手执行。
- OTA 升级把活动分区切到 `ota_1` 之后，`app-flash` 固定写入的 `0x10000`（`ota_0`）就不再是被启动的分区。
  继续开发前先执行 `idf.py -p COM3 erase-otadata` 让引导器回退到 `ota_0`（升级流程见「固件 OTA 升级」）。

想拿到不依赖构建目录的单一镜像，可以合并成从 `0x0` 起烧的文件：

```powershell
cd firmware
idf.py merge-bin -o remapad-firmware-merged.bin
```

结果写入 `firmware/build/remapad-firmware-merged.bin`，用 esptool 一次写入，不需要偏移参数：

```powershell
esptool --chip esp32s3 -p COM3 write-flash 0x0 remapad-firmware-merged.bin
```

乐鑫的 Flash Download Tool 也可以直接加载这个合并镜像。ESP-IDF 的 esptool 随 Python 环境安装，命令名是 `esptool`（`esptool.py` 在新版中已弃用）。
不确定端口时用 `Get-PnpDevice -Class Ports | Where-Object Status -eq OK` 列出当前串口。

## 自动化测试

三套测试都在开发机上跑，不需要真板；细节与回归规则见 [TESTING.md](TESTING.md)。

```powershell
cargo test --locked --manifest-path ui/Cargo.toml   # 屏幕 UI：编译真实 .slint 产物，按元素几何与像素断言
uv run python scripts/firmware-test.py              # 固件主机端：把纯逻辑模块编译成本机可执行文件并运行
cd pc ; uv run python -m unittest discover -s tests -t .   # PC 侧：串口枚举、镜像校验、帧编解码与输出分流
```

界面用例在开发机上跑真实 `.slint` 产物（同一套字体烘焙与软件渲染器）；
固件主机端测试会自动探测本机编译器（MSVC / clang / gcc，可用 `CC` 指定），几秒钟出结果。
界面排版与配色也可以先用 `uv run python scripts/ui-preview.py` 点着看，但预览不产生断言。

用例纪律（先写用例、确认红过再改、优先端到端、开发期间不跑端到端套件）与各套用例的位置、运行方式以 [TESTING.md](TESTING.md) 为准。

## 串口 CLI 与 PWR 按键


固件在唯一的 Type-C（USB-Serial/JTAG，主控制台）上提供行命令 CLI，验收时可以不碰屏幕。与 `idf.py monitor` 共用端口，二者不要同时打开。
项目自带 [pc/remapadctl.py](../pc/remapadctl.py)（桥接转发、命令行、实机截图与 OTA 都在同一个进程里），串口与帧编解码实现在 [pc/link.py](../pc/link.py)。
同一套会话还有图形入口 [pc/remapadgui.py](../pc/remapadgui.py)（`uv run python remapadgui.py`：选口连接、转发开关、日志、命令行、屏幕设置、截图与升级），
界面与命令行不要同时连同一个口；调试动作（连接键、屏幕操控、状态回读）在「命令」页，界面上不放它们的按钮。
依赖由 uv 管理（在 `pc/` 目录下执行，见 [pc/README.md](../pc/README.md)）：

```powershell
cd pc
uv run python remapadctl.py -p COM3 status          # 配对/角色/背光/息屏/运行时长/电池/版本/升级状态
uv run python remapadctl.py -p COM3 key a           # 注入 A 键（键名见下方说明）
uv run python remapadctl.py -p COM3 key l 800       # 注入 L 键并保持 800 ms
uv run python remapadctl.py -p COM3 key release     # 立即释放注入的按键
uv run python remapadctl.py -p COM3 ui on           # 手动进出屏幕操控模式（on / off，不带参数看状态）
uv run python remapadctl.py -p COM3 stick l 4095 2048   # 左摇杆推满右（0-4095 或 center）
uv run python remapadctl.py -p COM3 stick reset     # 两侧摇杆回中
uv run python remapadctl.py -p COM3 link            # 链路快照：广播地址、连接间隔（itvl，4 = 5ms）、特性启用（feat）与上报计数
uv run python remapadctl.py -p COM3 headset auto    # 耳机状态字节：auto 按输入设备派生，也可钉住 0xNN 做主机侧 A/B
uv run python remapadctl.py -p COM3 shot            # 请求一次实机截图（PC 侧拼齐后存 PNG）
uv run python remapadctl.py -p COM3 trace 40        # 逐帧打印渲染耗时、提交耗时、damage 像素数与矩形条数（不带参数 60 帧）
uv run python remapadctl.py -p COM3 fwver 2.0.0     # 改写上报给主机的手柄固件版本（0x10 查询与出厂块共用；不带参数看当前值）
uv run python remapadctl.py -p COM3 version         # 运行镜像版本与分区、是否待验证
uv run python remapadctl.py -p COM3 rollback        # 回滚到上一个可用镜像（仅待验证状态）
uv run python remapadctl.py -p COM3 backlight 60    # 背光并持久化
uv run python remapadctl.py -p COM3 screen off      # 息屏（on 恢复）
uv run python remapadctl.py -p COM3 mode host       # 切到 host：COM 口消失，日志与 CLI 改走 UART0
uv run python remapadctl.py -p COM3 pad             # 识别到的手柄：来源、家族、型号、命中布局行、兜底与透传状态
uv run python remapadctl.py -p COM3 usb             # USB host 状态：角色、设备、收报告与写回计数、日志出口
uv run python remapadctl.py -p COM3 relay 0         # 关掉同代透传（默认开），观察解析重编码路径
uv run python remapadctl.py -p COM3 connect         # 连接键：开连接窗口等主机连上来（未配对身份进配对流程）
uv run python remapadctl.py -p COM3 pairing start   # 配新主机：断开当前主机后进发现广播，等新主机搜索配对（stop 停止广播并断链）
uv run python remapadctl.py -p COM3 wake            # 开唤醒窗口：未连接时发 0x81 把休眠主机叫起来，已连接则断开让它重连
uv run python remapadctl.py -p COM3 adv auto        # 广播窗口内的形态（auto 默认按窗口来源 / wake / reconnect），实机 A/B 对账用
uv run python remapadctl.py -p COM3 ctrl            # 手柄配色（ctrl [body button accent grip]，四段 0xRRGGBB，持久化；无参回读）
uv run python remapadctl.py -p COM3 advaddr         # 广播地址形态（auto / public / random，不落盘），分辨主机是否按地址形态过滤
uv run python remapadctl.py -p COM3 advpdu          # 广播 PDU 形态（auto / legacy / extended，不落盘）
uv run python remapadctl.py -p COM3 --amiibo Alm.bin # 上传 amiibo 镜像到设备 storage 分区槽位（540 或 572 字节 dump；amiibo list / select 0 选用）
uv run python remapadctl.py -p COM3 poweroff        # 关机（释放电源锁存，仅电池供电有效）
uv run python remapadctl.py -p COM3 reboot          # 软重启回 COM 模式
uv run python remapadctl.py -p COM3 --log --seconds 20         # 只读设备日志 20 秒
uv run python remapadctl.py -p COM3 --log --reset --seconds 25  # 先复位再抓完整启动日志
uv run python remapadctl.py -p COM3 --shot --out shots\ui.png   # 抓实机截图并指定输出路径
uv run python remapadctl.py -p COM3 capture on        # 主机输出原始采集开（off 关）：震动/玩家灯/指令等主机输出经 0x12 帧回传
uv run python remapadctl.py -p COM3 --capture host-raw.log --seconds 30 --pad   # 抓 30 秒主机原始输出到文件（布局转换前），手柄转发照常
```

`key` 的键名为 `a b x y plus minus home capture c l r zl zr ls rs up down left right gl gr ui`。
默认保持 250 ms（`ui` 为 500 ms，盖过组合键 300 ms 的翻转阈值），最长 60000 ms；注入叠加在输入源之上。
`stick` 设定的一侧摇杆持续生效、未设定的一侧沿用输入源，因此可以分别推左摇杆与右摇杆做对照。`link` 打印当前身份一行：
对外广播地址、连接句柄、会话状态（idle / advertising / wait-pair / normal）、报告格式、已开启的通知通道、已发送报告数与凭证条数，配对与回连过程可以直接在串口上对账。

`ui` 与 `ui on` / `ui off` 对应手柄操控屏幕模式：
`key ui` 注入的就是 L1+R1+L3+R3 组合键（保持 500 ms，盖过 300 ms 的翻转阈值），进模式后方向键移动焦点、圆圈键等价于点按屏幕，再按一次组合键退出；
`ui on` / `ui off` 直接置位，不经过组合键判定，用来单独确认模式的开关与退出恢复。方向键在模式里分两个轴：
左右翻页（四叶草卡片无限轮播，到底再按从另一端继续），上下在当前页的可聚焦项之间走（同样循环到另一端）；
实机上按住 L1 / R1 与按左 / 右等价（注入用 `key l` / `key r`；组合键以 L1 + R1 起手，四键同按与两肩键同按都不发方向）。
进入模式会先向主机补发一帧全松开，捕获期间按原来的上报节奏续发同样的中性帧：玩家的按键不再上行，主机也不会因为上报流中断把手柄判成离线。
模式里用 `key up` / `key down` / `key left` / `key right` 移动焦点，`key a` 等价于点按屏幕（键名表的 `a` 就是私有格式的圆圈键位，与手柄上的圆圈键同一位）。

不带设备命令时进入桥接 + 交互模式：不是 `:` 开头的行按固件 CLI 命令发送（回复是 `ok`/`err` 单行，串口上同时会滚动固件日志），`:` 开头的是工具命令（`:help` 看清单，另有 `:shot` / `:log` / `:ota` / `:quit`）。
命令走产品控制面同一路径（`firmware/main/console/cli.c` → bridge），不产生第二套控制逻辑；同一个进程持有串口，因此桥接转发、命令行、截图与升级可以同时进行（`idf.py monitor` 仍与之互斥）。
`--log` 只读日志、不改任何状态，每行前缀是本次读取的相对时间（`--raw` 可去掉），便于把按键、长按这类人工动作和固件日志对上。注意两点：
USB-Serial/JTAG 的片内状态机把 CDC 的 DTR/RTS 当复位控制线解释——RTS 拉高即复位设备，DTR 与 RTS 同时拉高会让设备停在不再运行应用的状态；
`remapadctl.py` 用 Win32 API 打开端口并把两条线固定为低电平，因此打开、读取、关闭都不会复位设备（连续调用 `status`，uptime 会持续增长）。
需要复位时用 `--log --reset`（只脉冲 RTS），自己写 PC 端工具时按同样规则处理这两条线。抓包/监视工具与烧录、CLI 互斥，端口被占用时先结束占用进程（按 PID 精确清理，见常见问题）。

PWR 按键（`firmware/main/drivers/pwr_key.c`，采样 GPIO40）：**短按**息屏/亮屏（息屏只关背光，再按恢复持久化亮度）；
**长按 3-6 秒松开**是连接键（与配对页「连接」按钮同一个动作）：没有链路也不在广播时打开连接窗口（已配对发回连形态、未配对进配对流程），
有链路或正在广播时停止广播并断开（设备平时静默）。USB 角色切换只在模式页与串口 `mode` 里做。
长按到 3 秒时蜂鸣器（GPIO42，`drivers/buzzer.c`；LEDC 定时器与通道与背光分离，两者占空比互不覆盖）短鸣一声提示可以松开；按住超过 6 秒不产生软件事件。
完全关机后重新上电（按 PWR 或插上 USB）时蜂鸣器同样短鸣一声作开机反馈，判据是复位原因 `power-on`：软件复位、OTA 重启与看门狗复位都不响。
SYS_EN（GPIO41）电源保持脚由固件在 `app_main` 入口最先拉高锁存：电池供电时松开 PWR 键后系统继续工作，复位窗口也不会掉电；USB 供电下锁存被旁路，拉高无副作用。
软件关机走系统页「关机」按钮（bridge 的 `powerOff` 命令，串口对应 `poweroff`）：电池供电下释放锁存即断电，USB 供电下锁存被旁路、关不掉，固件重新锁存后界面提示「USB 供电下无法关机，请拔线后再试」。

静默省电：设备完全静默（没有链路、没有广播窗口、不在配对流程）持续约 1 秒后关闭整个 BLE 栈——控制器断电、射频不再发热；
同一时刻起数据面采样与上报、界面状态轮询与动画推进一起降到 12 fps 等效节拍（83 ms），屏幕照常显示、不自动熄灭。
按连接键、未连接时按 HOME、发起配对新主机都会重新起栈并按对应意图广播，用户不必按第二次（起栈要付一次控制器初始化时间）。
串口日志里对应 `ble stack idle: shutting the controller down` 与 `ble stack start on demand` 两行。

用户设置（背光亮度、手柄四段配色、上报固件版本）持久化在 NVS（`firmware/main/config/app_config.c`），重启后恢复；息屏状态与 USB 连接模式不跨重启保留（USB 角色开机恒为串口）。
PC 手柄经桥接程序进入设备这条路径已落地：设备侧见 `firmware/main/input/`，PC 侧见 [pc/README.md](../pc/README.md)；
手柄插在板卡上的 USB host 直插也已落地（`firmware/main/usb/`）。

## 固件 OTA 升级

整包应用镜像（界面已编在应用里）可以在不接线烧录的情况下升级：PC 端把镜像经 USB-Serial/JTAG 推给设备，设备写进当前未运行的应用分区，`esp_ota_end` 校验通过后切换启动分区并重启。
设备侧实现在 `firmware/main/ota/`，PC 端入口是 [pc/remapadctl.py](../pc/remapadctl.py) 的 `--upgrade`：

```powershell
cd pc
uv run python remapadctl.py --dry-run                     # 只校验镜像，不接设备
uv run python remapadctl.py -p COM3 --upgrade             # 升级默认镜像 ../firmware/build/remapad_firmware.bin
uv run python remapadctl.py -p COM3 --upgrade --image ..\firmware\build\remapad_firmware.bin
uv run python remapadctl.py -p COM3 --upgrade --wait      # 升级后等设备重启回来并打印新版本
uv run python remapadctl.py -p COM3 --upgrade --verbose   # 同时透传设备日志
```

要点：

- 升级前先跑 `idf.py build`，镜像就是 `firmware/build/remapad_firmware.bin`；
  设备只接受项目名为 `remapad_firmware` 的 ESP32-S3 应用镜像，尺寸上限是应用分区容量 4 MB。
 升级由持有 COM 口的那个进程执行：`remapadctl.py --upgrade` 自己就是持有者，桥接转发与命令行在同一会话里照常；先退出 `idf.py monitor` 等其它占用进程，设备必须处于串口模式（host 模式下 COM 口不存在）。
  升级与设备当前是否连着 NS2 主机无关，重启后按凭证回连。
- 校验通过后设备自动重启，首次启动处于「待验证」状态：UI 首帧成功且稳定运行满 30 秒才标记为有效，在此之前断电或重启会自动回退到升级前的镜像，此时 `version` 显示 `image=pending-verify`。
- 升级中断（PC 退出、拔线、断电）不影响启动：`otadata` 在成功前不动，设备仍从旧镜像启动，残留在另一个分区的半镜像会在下次升级时重新擦写。
- 从 `ota_1` 启动之后，开发期 `app-flash` 固定写 `0x10000`（`ota_0`）未必是当前启动分区。
  继续开发前先执行 `idf.py -p COM3 erase-otadata`（引导器随后回退到 `ota_0`）。

## 关键文件

- [ui/src/](../ui/src)：界面源码（`app.slint` 页表与根窗口、`pages.slint`、`components.slint`、`theme.slint`）。
- [ui/assets/](../ui/assets)：字体（正文 / 图标 / 转圈）与卡片、底栏底图 SVG。
- [ui/host/tests/](../ui/host/tests)：界面宿主用例（界面测试后端 + 软件渲染器）。
- [ui/slint_ui/](../ui/slint_ui)：固件 Rust 界面组件：平台层、宿主层、C ABI、装配层 `ui_host.c` 与启动画面 `boot_splash.c`。
- [ui/build-support/](../ui/build-support)：宿主用例与固件组件共用的界面编译口径（风格 / 字号表 / 字体）。
- [firmware/main/ui/](../firmware/main/ui)：UI 契约的 core 侧（`ui_service.h` 契约、状态快照装配与动作分发；无 UI 构建另有空实现 stub）。
- [firmware/main/config/app_config.c](../firmware/main/config/app_config.c)：用户设置 NVS 持久化（亮度 / 连接模式 / 手柄身份）。
- [firmware/main/console/cli.c](../firmware/main/console/cli.c)：串口行命令 CLI（USB-Serial/JTAG）。
- [firmware/main/drivers/pwr_key.c](../firmware/main/drivers/pwr_key.c)：PWR 按键采样（短按息屏、长按是连接键）。
- [firmware/main/dp/dp_source.c](../firmware/main/dp/dp_source.c)：数据面输入源抽象（注册制；桥接源在 `input/`，USB host 源在 `usb/`）。
- [firmware/main/usb/](../firmware/main/usb)：USB host 直插（枚举与 HID 收发、输入源、运行时角色切换）与主机反馈写回。
- [firmware/main/pad/feedback.c](../firmware/main/pad/feedback.c)：反馈编码（按设备布局行把震动 / 玩家灯编码成该手柄的输出报告）；
  采样音色表按音色段驱动板载蜂鸣器发声（USB 直插），蓝牙桥接路径直接丢弃采样。
- [firmware/main/input/input_link.c](../firmware/main/input/input_link.c)：桥接链路的设备侧（USB-Serial/JTAG 唯一读取者、桥接帧与 CLI 文本分流）。
- [firmware/main/pad/pad_device.c](../firmware/main/pad/pad_device.c)：私有手柄格式与解析（按键位置映射、轴归一、死区）；
  家族布局表按系列拆在 [firmware/main/pad/layouts/](../firmware/main/pad/layouts)。
  契约与注册表是 [layout.h](../firmware/main/pad/layout.h) / [layout.c](../firmware/main/pad/layout.c)。
- 目标编码接口 [target.c](../firmware/main/target/target.c) 与 NS2 输出封装 [ns2/](../firmware/main/target/ns2)：
  涵盖按键构建报告、结构化反馈、电池与 amiibo 预置。
- [pc/remapadctl.py](../pc/remapadctl.py) 与 [pc/link.py](../pc/link.py)：
  PC 侧单工具（hidapi 读手柄 → 桥接帧、串口命令行、实机截图与 OTA 在同一个进程里；`--dump` 核对家族表偏移；依赖与运行方式见 [pc/README.md](../pc/README.md)）。
- [pc/remapadgui.py](../pc/remapadgui.py)：同一套会话的图形界面（CustomTkinter；输出走可注入的 Reporter、命令由按钮与输入框投递）；
  「设置」页把设备屏幕上的可改项搬到 PC（读写都走固件 CLI，控件值来自回读行）。
- [firmware/main/ota/](../firmware/main/ota)：OTA 升级会话与协议（分区回写、窗口流控、回滚健康门槛），PC 端入口是 `remapadctl.py --upgrade`。
- [firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)：Flash/PSRAM、CPU 频率、FreeRTOS 与主控制台（USJ）预设。
- [firmware/partitions.csv](../firmware/partitions.csv)：NVS、PHY、OTA 双应用分区和通用存储区（storage）的终局布局。
- [scripts/](../scripts)：`setup-rust-toolchain.py`（工具链）、`ui-preview.py`（预览）、`firmware-test.py`（主机端用例）、`create_adr.py`（新建 ADR）。
- [agent-temp/](../agent-temp)：代理与调试的临时文件目录（脚本、抓包输出、截图与日志；内容不进版本库，约定见 [AGENTS.md](../AGENTS.md)）。
- [docs/controller-switch2.md](controller-switch2.md)：NS2 手柄广播、GATT、HID 报告、配对、指令集与 NFC 规范。
- [docs/controller-ps.md](controller-ps.md)：DS3 / DS4 / DualSense 的输入输出报告、触觉通路与行为设置。
- [docs/hardware.md](hardware.md)：目标板卡的 SoC/存储、屏幕、触摸、外设、GPIO 分配和板级注意事项。

### USB 手柄直插（host 模式）

手柄插在板卡 Type-C 上时设备做 USB 主机：`firmware/main/usb/` 装 host 栈、按报告描述符挑手柄用途的 HID 接口（跳过厂商与音频接口）、收 IN 报告后按 VID/PID 走同一份家族布局表。
真 Switch 2 手柄的报文体原样转发给 NS2 主机（真电量与真陀螺仪直达），其余家族解析成私有格式后重新编码；主机下发的震动与玩家灯按布局行编码写回手柄的 OUT 端点。

切换入口有两个：模式页「手柄」卡片、串口 `mode host`；角色只在本次运行有效、不写 NVS。
切过去之后在 UART0 上敲 `pad` 看识别结果与是否透传，敲 `usb` 看 host 栈状态与收发计数。

host 模式下的排查只有一条通道：板卡只有一根 Type-C，进了 host 就没有 COM 口，串口 CLI、桥接程序与 OTA 全部用不了，日志与 CLI 只剩 UART0——
扩展焊盘 **GPIO43（TX）/ GPIO44（RX）接 3.3V USB-UART 适配器**，115200 8N1、GND 共地；固件在切 host 之前先把日志与 CLI 出口迁到那里，接上适配器就能看到切换全过程与手柄枚举日志。

没有适配器时的替代只有三条：看屏幕（系统信息页、底栏手柄状态、模式页选中的角色）、整机复位（复用开关默认接 USB-Serial/JTAG，COM 口与烧录链路天然回来）、
或先切回串口再在 PC 上查——host 期间的日志没有缓冲，切回来补看不到。

- 回到串口有三条路：模式页切回「串口」、UART0 上敲 `mode device`、复位。
  切回时固件把内部 PHY 显式交还 USB-Serial/JTAG，COM 口随之回来；这一步失败时只有复位能恢复，
  界面因此在切回后询问是否立刻重启。
- 识别结果看 `pad`（家族、VID:PID、命中的布局行、兜底标记、是否透传）与 `usb`（枚举到的设备、报告与写回计数）；未登记的 VID/PID 回落 XInput 形态布局并打兜底标记。
- 供电：host 模式要给插入的手柄供 VBUS 5V，V2.1 原理图确认板上无升压输出，需从 TP1 外部注入 5V（见 [hardware.md](hardware.md)）；手柄能否枚举仍需实机验证。

## 常见问题

### 预览时报找不到 `slint-viewer`

预览工具不是本仓库的依赖，要单独装一次（装法与版本口径见 [ui/README.md](../ui/README.md)）。
装完重开终端让 `%USERPROFILE%\.cargo\bin` 进 `PATH`；脚本给的提示里就是这条命令。

### 屏幕上中文显示为方框（tofu）

中文字形是否可用取决于构建期烘出来的字形位图：构建脚本把 `assets/fonts/NotoSansSC-Regular.otf` 作为默认字体，
界面里出现过的中文会被烘进各字号槽位。仍显示方框或空洞的常见原因：
文本是运行期拼出来的、字符从未出现在任何界面源码字面量里（把码点加进 `ui/src/app.slint` 的锚点串），
或用了字体不覆盖的码点（emoji 没有字形，只能显示为方框）。改完文案重新 `idf.py build` 即可。

### `cargo +esp 不可用` 或 `工具链缺 rust-src 组件`

配置阶段的这两条报错来自 `ui/slint_ui/CMakeLists.txt` 的工具链自检（报错里同时给出 `-DREMAPAD_UI=OFF` 的出路），按提示修：

```powershell
uv run python scripts/setup-rust-toolchain.py               # 装 Xtensa 工具链
rustup component add rust-src --toolchain esp               # 缺 rust-src 时
```

工具链装在别的名字下时设 `$env:REMAPAD_SLINT_RUST_TOOLCHAIN` 再 `idf.py reconfigure`（见 [ui/README.md](../ui/README.md)）。

### `idf.py build` 秒退且没有编译输出

现象是打印几行环境提示后出现下面这条错误，紧接一行 `Executing action: all (aliases: build)` 就结束，退出码 0、`firmware/build/` 里的产物时间戳不变：

```text
'<venv>\python.exe' is currently active in the environment while the project was configured with '<venv>\python.exe'
```
这不是构建成功：`firmware/build/CMakeCache.txt` 记录了配置工程时用的 Python 解释器（`rg -n '^PYTHON' firmware/build/CMakeCache.txt`）。
换到另一套 IDF 环境后 `idf.py` 只做校验就返回，一步都不编译。按提示里的两条路径切回配置工程时用的那套环境即可；
要换环境长期使用则在当前环境里 `idf.py fullclean` 后重新配置（见「4. 编译 ESP-IDF 固件」）。想确认工程到底有没有活干，可以先试运行一次 ninja，它只列步骤、不改文件：

```powershell
ninja -C firmware\build -n
```

### 烧录后没有屏幕画面

面板由 `drivers/panel.c` 驱动（esp_lcd 内置 ST7789，SPI2 80 MHz，见 [ARCHITECTURE.md](ARCHITECTURE.md) 的显示通路预算）。正常时序是：
界面提供者任务在面板与触摸初始化成功后自绘启动画面（`ui/slint_ui/boot_splash.c`），背光随启动画面落屏由 `drivers/backlight.c` 点亮，随后每个启动阶段推进一次进度条；
界面首帧提交成功后启动画面交出屏幕并释放缓冲。若画面不可见，先看串口日志：`panel init failed` 表示面板初始化失败（此时固件跳过启动画面，画面仍渲染进 PSRAM）；
有启动画面日志但屏幕黑，再检查背光（`GPIO15` 需要显式驱动，若 `backlight init failed` 会有对应日志）与面板排线；日志里没有启动画面但 UI 正常，说明是从旧镜像启动，重新烧录即可；
无 UI 构建（`REMAPAD_UI=OFF`）不初始化面板与背光，屏幕保持熄灭是预期行为（日志里有一句 `no-UI build`）。
修改面板方向/偏移配置时要对照 [hardware.md](hardware.md) 与微雪官方示例，不要凭空猜测初始化序列。

### `ui start failed (internal=… largest=… psram=…) `

这条日志表示 UI 平台建不起来，括号里的三个数字是当时的空闲内存。平台要两块钱：
整帧缓冲 240 × 280 × 2 字节（约 134 KB，进 PSRAM）与行带缓冲 240 × 48 × 2 字节（约 23 KB，要内部 RAM 且 DMA 可达），
另外界面任务的 64 KB 栈也在内部 RAM。largest 明显小于 23 KB 时先看谁把内部 RAM 占住了。

### 启动时崩溃重启，崩溃位置每次都不一样

这类「位置漂移」的崩溃通常不是空指针，而是某个任务写穿了自己的栈。界面的渲染路径在界面任务的 64 KB 栈上跑，
改平台层（`platform.rs`）或加深界面嵌套以后，先把栈量一遍再往上加代码；不要只调大行带缓冲而不看栈。

### 启动阶段出现 `task_wdt` 告警

从 `app_main` 到首帧就绪之间的阶段权重是 60 / 60 / 80 / 20 ms（面板与平台、建界面、首帧、进事件循环），
正常情况几秒钟内交屏，不该长时间占住一个核。持续告警时先看是不是卡在面板初始化或某次超长传输上
（`trace` 逐帧打印渲染与提交耗时）。`CONFIG_ESP_TASK_WDT_PANIC` 没有开启，告警本身不会重启设备。

### 运行时反复 `task_wdt` 告警并且界面掉帧

先看界面任务每 5 秒一条的统计：`frames=… avg_render_us=… avg_flush_us=… avg_damage_px=… max_render_us=… max_flush_us=…`。
渲染与提交之和接近或超过 16 ms 时，说明每帧把整个周期都吃满了，空闲任务自然喂不上狗。
先用串口 `trace 60` 看清是哪一类帧贵：`damage_px` 大说明重画范围大，`flush_us` 大说明面板传输慢（行带太碎或 SPI 争用）。
CPU 频率在 `firmware/sdkconfig.defaults` 里显式配置为 240 MHz；降不下来就要从界面写法入手。

### `Could not open COM3, the port is busy`

多半是上一轮 `idf.py monitor` 的 python 进程没退干净，占着串口。按进程精确清理后再烧录：

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -like '*idf_monitor*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

不要按 `python.exe` 之类的进程名批量结束，这些是多个项目共用的进程。

### 屏幕上没有出现 BLE 手柄广播

BLE 手柄外设已接入（`firmware/main/ble/`）：
开机后设备以厂商数据广播出现（nRF Connect 可见 Company ID `0x0553`），主机互操作已在 Switch 2 实机对账——发现、连接、0x15 配对、断连回连与 HOME 唤醒均实测通过，协议依据与对账记录见 [controller-switch2.md](controller-switch2.md)。
排查顺序：先看启动日志有无 `host synced` 与 GATT 句柄表，再确认广播载荷，最后对照 [controller-switch2.md](controller-switch2.md) 逐段核对。
USB host 直插与 PC 桥接两条输入路径都已接入（`firmware/main/usb/`、`firmware/main/input/`），调试页的注入按钮仍可合成按键。

### 预览里点了没反应

预览是纯前端渲染，控件动作都经 `action` 回调交回固件，预览里没有固件接这些回调，因此点按不会切页、也不会改设置。
想在预览里看别的页面，就在 slint-viewer 的属性面板里改 `page`（可选属性都挂在 `App` 上）。

### 实机上触摸无效

设备端触摸由 `drivers/touch.c` 采样 CST816T，触点经 `hooks.touch_sample` 交给界面。
先看启动日志有无 `touch init failed`（多为 I2C 无应答，检查地址 `0x15` 与共享总线接线）；init 失败时固件继续运行，但每帧触点为零。
息屏期间触摸整段跳过，按 PWR 键或发命令亮屏后再试。

### `firmware/build` 或 `ui/target` 出现文件

这两个目录是生成目录，已被 Git 忽略。不要手动编辑其中的 Rust 生成代码、静态库或镜像。

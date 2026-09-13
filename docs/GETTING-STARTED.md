# Remapad 新手开发与上手指南

本指南面向微雪 ESP32-S3-Touch-LCD-1.69 目标板，说明 UI 检查、PocketJS 包构建、ESP-IDF 编译和当前 bring-up 边界。Remapad 的最终产品链路是 USB 输入→NS2 手柄报告→BLE 输出，并通过屏幕 UI 管理连接和配对；协议资料见 [controller.md](controller.md)，板卡规格与引脚见 [hardware.md](hardware.md)。

## 前置环境

| 工具 | 版本/要求 | 用途 |
| :--- | :--- | :--- |
| Node.js | 18 或更高 | 运行项目脚本和已发布 CLI |
| pnpm | 当前稳定版 | 工作区依赖与任务调度 |
| Bun | PocketJS 官方要求的版本 | 执行官方 compiler、官方构建脚本和 Web 开发主机 |
| PocketJS compiler | 仓库内的 `ui/vendor/pocketjs` 快照 | 提供 `tools/pocket.ts` 与 ESP-IDF host profile 支持；npm 上发布的 0.11.0 尚不含该支持 |
| Xtensa Rust | `esp-rs/rust-build` 的 `v1.97.0.0` | 仅在升级组件、重新生成 ESP32-S3 原生归档时需要 |
| Python | 由 ESP-IDF 安装环境提供 | `idf.py`、ESP-IDF 工具链和官方 package 嵌入步骤 |
| ESP-IDF | `>=6.0,<6.2` | PocketJS 官方 ESP-IDF 组件要求；本仓库已在 6.1 上验证 |
| 硬件 | 微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8） | 16 MB Flash、8 MB Octal PSRAM、240 × 280 ST7789V2 触摸屏；细节见 [hardware.md](hardware.md) |

USB 输入设备、目标 NS2 手柄型号和 BLE 天线/射频属于最终硬件范围，但当前仓库尚未完成这些产品 BSP。不要因为 Web 预览可以交互就认为真实 USB 或 BLE 链路已经可用。

板卡已知信息都记录在 [hardware.md](hardware.md)：屏幕为 ST7789V2（240 × 280，4-wire SPI），触摸为 CST816T（I2C `0x15`），面板和触摸的具体引脚、共享 I2C 总线、背光控制脚和 USB 口约束都在那里。固件已通过 `drivers/` 中的 panel/touch/backlight BSP 点亮屏幕并上报触点（选型见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)）；BLE 手柄数据面已接入（[ADR 0010](adr/0010-nimble-ble-controller-stack.md)、[ADR 0011](adr/0011-controller-dataplane-module-boundary.md)，进度见 [ROADMAP.md](ROADMAP.md)）但主机互操作待实机验证；USB 输入与电池等其余外设仍待实现。

## 最短步骤

### 1. 安装依赖

```powershell
pnpm install
```

六个官方 ESP-IDF 组件与 ESP32-S3 原生归档已随仓库固定在 `firmware/components/`，Web 开发主机随 `ui/node_modules/@pocketjs/framework` 一起安装，因此这一条之后就只剩构建命令。

前端检查、编译和打包使用仓库内的 `ui/vendor/pocketjs` 快照，依赖由 `pnpm install` 安装，因此不需要额外准备。快照的来源与同步方式见 [ui/vendor/pocketjs/README.md](../ui/vendor/pocketjs/README.md)。

### 2. 准备 PocketJS ESP-IDF 依赖（升级时）

组件随仓库提供，日常开发不需要这一步。只有在登记上游 PocketJS 更新、或 ESPComponentRegistry 的 `espressif/quickjs-ng` 内容变化时，才需要重新对账：

1. 把上游 `hosts/esp-idf/components/` 的最新源码同步进本仓库的 `firmware/components/`。
2. 用固定版本的 Xtensa Rust 重新生成 ESP32-S3 原生归档：

```powershell
# 指向 esp-rs/rust-build v1.97.0.0 的 cargo，路径按本机安装位置调整
$env:POCKETJS_CARGO = "$env:USERPROFILE\.esp-rust\1.97.0.0\bin\cargo"
pnpm run native
```

3. 核对 `pocketjs_guest` 的 QuickJS 源码校验值。本仓库固定的副本已经使用 Registry 当前的哈希；若上游换用新的 `espressif/quickjs-ng`，按 [patches/README.md](../patches/README.md) 重新记录，并同步 `build-receipt.json` 中的编译器信息。

`pnpm run native` 需要能访问 PocketJS 源码（`POCKETJS_ROOT` 或仓库同级 `../pocketjs`），产物直接写入 `firmware/components/*/lib/esp32s3/`；`idf.py build` 本身不需要 Rust。

### 3. 检查 UI 与设备契约

```powershell
pnpm run lint
pnpm run check
```

`check` 会使用 `ui/pocket.json` 和 `firmware/pocket.host.json`，由官方 resolver 检查 manifest、能力、视口、tick 和 host profile。它不修改 UI 包。

### 4. 编译 UI 资源与 `.pocket`

```powershell
pnpm run compile
pnpm run build
```

脚本最终调用 PocketJS 官方 CLI，输出到 `ui/dist/`：

```text
remapad-ui.js       编译后的 JavaScript bundle
remapad-ui.pak      样式、字体和图像资源包
remapad-ui.pocket   面向 remapad-s3 host profile 的单文件包
```

`scripts/pocketjs.mjs` 按 `POCKETJS_ROOT`、`ui/vendor/pocketjs`、仓库同级 `../pocketjs` 的顺序定位包含 `--host-profile` 的官方脚本；默认命中仓库内的快照。它只负责路径与参数转发、建立快照的依赖链接，不实现 compiler，也不改变 package 格式。

快照同时携带编译器生成的 `framework/src/styles.generated.ts`（class 字面量到 styleId 的映射）。它必须提交：官方 CLI 的类型检查跑在编译器写入该文件之前，而 `pnpm install` 之后新写入的快照文件不会进入 pnpm 的依赖副本，缺少它时连 `pnpm run check` 都会以 TS2307 失败。每次 `pnpm run build` 会按当前 `ui/src` 重新生成该文件，出现差异时正常提交即可。

官方命令的语义如下，`pocket build` 的 `--host-profile` 形式等价于上面的项目脚本：

```powershell
cd ui/vendor/pocketjs
bun tools/pocket.ts build --manifest ../pocket.json `
  --host-profile firmware/pocket.host.json `
  --project-root ui --outdir ui/dist `
  --output ui/dist/remapad-ui.pocket
```

不要将 `--target psp` 用在本项目上。`psp` 是 Sony PSP 后端的 target 名称；ESP32 使用自定义 `--host-profile`。

### 5. 触摸屏预览

```powershell
pnpm run dev
```

该命令先用官方 `compile` 把 bundle 与 PAK 写入 `ui/dist/`，再启动项目内的触摸预览页。打开 [http://127.0.0.1:8130](http://127.0.0.1:8130) 可以看到 240 × 280 屏幕、触摸输入和运行读数。同一命令还会用快照内的官方 `hosts/web/serve.ts` 拉起官方 DevTools 服务器，预览页的「DevTools 面板」按钮会打开 [http://127.0.0.1:8131/devtools](http://127.0.0.1:8131/devtools)：组件树与屏幕高亮、暂停/单步、console 镜像与 REPL、输入磁带导出/重放/时序回退、截图，全部由官方面板与 hub 承载，预览页只负责按官方 `engine.js` 的设备协议接入 `/ws`。

注意浏览器会把后台标签页的 `requestAnimationFrame` 停掉：聚焦面板标签页时预览页（后台）帧循环暂停，面板数据随之静止。请使用并排窗口或双显示器让预览页保持可见，或者只在暂停调试时聚焦面板。

交互方式按设备的触摸屏设计：在屏幕上按下、拖动、抬起即可，没有虚拟按键和键盘映射。预览页把指针事件转换为官方触摸帧契约（`frame(buttons, analog, touches, hits)`），触点坐标使用逻辑像素，命中事实在按下瞬间查询一次，因此点击、拖动和手势与真机走同一套判定逻辑。

预览页是项目自己的页面（`ui/preview/index.html` + `scripts/preview-server.mjs`），渲染核心和触摸语义来自官方 `@pocketjs/framework` 的浏览器运行时；官方 playground 面向 PSP 按键，本项目不使用它。首次运行需要 Rust 的 `wasm32-unknown-unknown` target 构建 `pocketjs.wasm`，之后直接复用。

面板读数中的「触摸帧」是含触点的帧数，「命中节点」是按下时的命中结果，可用于确认触摸链路是否正常。

面板的「屏幕尺寸」开关在原始尺寸（1×）与放大尺寸（2×）之间切换，也可以按屏幕实际像素观察绘制结果。它只改变浏览器里的显示大小，触点坐标按画布实际尺寸换算，切换尺寸不影响触摸判定；选择记录在浏览器本地，刷新后保留。

### 6. 编译 ESP-IDF 固件

从 ESP-IDF PowerShell 或已加载 `export.ps1` 的终端执行：

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
```

`firmware/components/` 中的官方组件由 ESP-IDF 自动发现，`firmware/main/CMakeLists.txt` 按顺序接入包：

1. 如果 `ui/dist/remapad-ui.pocket` 存在，使用官方 `pocketjs_embed_package`。
2. 否则使用官方 `pocketjs_compile_app`，让 CMake 调用 PocketJS CLI 生成 build 目录内的包。

建议先运行 `pnpm run build`，再运行 `idf.py build`。预构建路径只需要 Python 执行官方嵌入脚本，不需要 Bun；编译路径则需要可被 CMake 找到的官方 `pocket` CLI 和 Bun。

嵌入过程把 `ui/dist/remapad-ui.pocket` 登记为 CMake 依赖，所以改完 `ui/src` 之后重新执行 `pnpm run build` 与 `idf.py build`，固件会自动重新嵌入新包，不需要删除 `firmware/build/`。

### 7. 烧录与监视

```powershell
idf.py -p COM3 flash monitor
```

没有 ESP-IDF 终端时（例如从 Git Bash 直接发起），可以用一条 PowerShell 命令激活 EIM 环境后执行。关键是先清掉 `MSYSTEM`——Git Bash 会把它带给子进程，`idf.py` 检测到后只打印警告并静默拒绝执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'; Set-Location firmware; idf.py -p COM3 flash"
```

EIM 安装路径不同时改用实际的 `Microsoft.*.PowerShell_profile.ps1` 位置即可。

把 `COM3` 替换为实际端口。若开发板没有自动进入下载模式，按板卡说明操作 BOOT/EN。串口监视器使用 `Ctrl + ]` 退出。

`idf.py build` 在 `firmware/build/` 下生成三个可烧录文件，偏移与 `firmware/build/flash_project_args` 一致：
| 文件 | 烧录偏移 |
| :--- | :--- |
| `build/bootloader/bootloader.bin` | `0x0` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/remapad_firmware.bin` | `0x10000` |

应用镜像已经内嵌 `.pocket` 包，烧完这三个文件就是完整的设备固件。

日常迭代只改应用层（`ui/` 产物或 `firmware/main/`）时，bootloader 和分区表没有变化，可以只重写 `0x10000` 处的应用分区，比整片烧录快，对 Flash 的擦写也更少：

```powershell
idf.py -p COM3 app-flash monitor
```

改动 bootloader、分区表或 `sdkconfig` 后仍需完整 `flash`。esptool 的等价操作是对 `0x10000` 单独 `write-flash`。

分区表自 ADR 0009 起为终局布局（`ota_0`/`ota_1` 双应用分区 + `storage` 通用存储区，`ota_0` 继承原 factory 的 `0x10000`）。烧录时注意：

- 分区表布局变更后烧录分区表即可让已部署固件原地迁移为 `ota_0`，但保险起见直接完整 `flash`。
- `erase-flash` 会全片擦除，清掉 NVS 里的设置与 BLE 配对、`storage` 里的用户数据；设备交到用户手上之后不要再随手执行。
- 将来 OTA 运行期把活动分区切到 `ota_1` 后，`app-flash` 固定写入的 `0x10000`（`ota_0`）未必是被启动的分区，继续开发前先擦除 otadata。

想拿到不依赖构建目录的单一镜像，可以合并成从 `0x0` 起烧的文件：

```powershell
cd firmware
idf.py merge-bin -o remapad-firmware-merged.bin
```

结果写入 `firmware/build/remapad-firmware-merged.bin`，用 esptool 一次写入，不需要偏移参数：

```powershell
esptool --chip esp32s3 -p COM3 write-flash 0x0 remapad-firmware-merged.bin
```

乐鑫的 Flash Download Tool 也可以直接加载这个合并镜像。ESP-IDF 的 esptool 随 Python 环境安装，命令名是 `esptool`（`esptool.py` 在新版中已弃用）。不确定端口时用 `Get-PnpDevice -Class Ports | Where-Object Status -eq OK` 列出当前串口。

## 串口 CLI 与 PWR 按键

固件在唯一的 Type-C（USB-Serial/JTAG，主控制台）上提供行命令 CLI，验收时可以不碰屏幕。与 `idf.py monitor` 共用端口，二者不要同时打开。项目自带 [scripts/uartctl.py](../scripts/uartctl.py)（依赖 pyserial）：

```powershell
python scripts/uartctl.py -p COM3 status          # 配对/角色/背光/息屏/运行时长
python scripts/uartctl.py -p COM3 key a           # 注入 A 键（home / lr 同理）
python scripts/uartctl.py -p COM3 backlight 60    # 背光并持久化
python scripts/uartctl.py -p COM3 screen off      # 息屏（on 恢复）
python scripts/uartctl.py -p COM3 mode host       # 连接模式（otg 被固件拒绝）
python scripts/uartctl.py -p COM3 pairing start   # 配对广播开关
python scripts/uartctl.py -p COM3 reboot          # 软重启回 COM 模式
```

不带命令进入交互模式；命令回复为 `ok`/`err` 单行，串口上同时会滚动固件日志。命令走产品控制面同一路径（`firmware/main/console/cli.c` → bridge），不产生第二套控制逻辑。注意两点：打开 USB-Serial/JTAG 口通常会把设备复位一次（USJ 特性），所以每次 `uartctl.py` 调用后 `uptime` 会归零属正常现象，连续操作建议用交互模式；抓包/监视工具与烧录、CLI 互斥，端口被占用时先结束占用进程（按 PID 精确清理，见常见问题）。

PWR 按键（`firmware/main/drivers/pwr_key.c`，采样 GPIO40）：**短按**息屏/亮屏（息屏只关背光，再按恢复持久化亮度）；**长按 3-6 秒松开**切换连接模式（device ↔ host，桥接 otg 双端禁切，防止 USB PHY 切走后 COM 消失无法烧录）。长按到 3 秒时蜂鸣器（GPIO42，`drivers/buzzer.c`）短鸣一声提示可以松开；按住超过 6 秒不产生软件事件。SYS_EN（GPIO41）电源保持脚暂不驱动：USB 供电下锁存被旁路，电池供电场景待电源 BSP 阶段接入。

用户设置（背光亮度、连接模式、手柄类型与配色）持久化在 NVS（`firmware/main/config/app_config.c`），重启后恢复；息屏状态不跨重启保留。USB 输入与桥接模式的推进方案（当前仅架构预留）见 [usb-input-plan.md](usb-input-plan.md)。

## 关键文件

- [ui/pocket.json](../ui/pocket.json)：应用清单和应用侧 capability。
- [ui/src/fonts.json](../ui/src/fonts.json)：应用目录的回退字体清单；中文字体烘焙见 [ui/assets/fonts/](../ui/assets/fonts)。
- [firmware/pocket.host.json](../firmware/pocket.host.json)：ESP32-S3 host profile。
- [firmware/components/](../firmware/components)：固定的官方 ESP-IDF 组件与 ESP32-S3 原生归档。
- [firmware/main/CMakeLists.txt](../firmware/main/CMakeLists.txt)：官方 package embed/compile 接入。
- [firmware/main/pocketjs_host.c](../firmware/main/pocketjs_host.c)：package、guest、binding、renderer 生命周期与 `remapad-pjs` owner task。
- [firmware/main/boot_splash.c](../firmware/main/boot_splash.c)：UI 就绪前的固件自绘启动画面（主题底色 + 手柄标记 + 阶段进度条），同时在启动画面落屏后提前点亮背光。
- [firmware/main/config/app_config.c](../firmware/main/config/app_config.c)：用户设置 NVS 持久化（亮度 / 连接模式 / 手柄身份）。
- [firmware/main/console/cli.c](../firmware/main/console/cli.c)：串口行命令 CLI（USB-Serial/JTAG）。
- [firmware/main/drivers/pwr_key.c](../firmware/main/drivers/pwr_key.c)：PWR 按键采样（短按息屏、长按切模式）。
- [firmware/main/dp/dp_source.c](../firmware/main/dp/dp_source.c)：数据面输入源抽象（注册制，USB 源预留）。
- [firmware/main/ns2/ns2_output.c](../firmware/main/ns2/ns2_output.c)：NS2 输出封装（按键构建报告、结构化反馈、电池、amiibo 预置）。
- [firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)：Flash/PSRAM、CPU 频率、FreeRTOS 与主控制台（USJ）预设。
- [firmware/partitions.csv](../firmware/partitions.csv)：NVS、PHY、OTA 双应用分区和通用存储区（storage）的终局布局（ADR 0009）。
- [scripts/pocketjs.mjs](../scripts/pocketjs.mjs)：编译器、触摸预览和原生归档脚本的统一入口。
- [scripts/uartctl.py](../scripts/uartctl.py)：串口 CLI 的 PC 端客户端。
- [ui/preview/index.html](../ui/preview/index.html)：触摸屏预览页与触摸帧契约实现。
- [patches/README.md](../patches/README.md)：与上游组件的差异记录、QuickJS 校验值核对与升级步骤。
- [docs/controller.md](controller.md)：NS2 手柄 USB/BLE、广播、GATT、HID 报告和配对规范。
- [docs/hardware.md](hardware.md)：目标板卡的 SoC/存储、屏幕、触摸、外设、GPIO 分配和板级注意事项。
- [docs/usb-input-plan.md](usb-input-plan.md)：USB 输入接收与桥接模式方案预案（仅架构）。

## 最终产品数据面（当前规划）

后续固件工作按以下顺序拆分（进度跟踪见 [ROADMAP.md](ROADMAP.md)，BLE 链路先行、USB 输入殿后）：

1. 接入 ESP-IDF USB host，接收并解析输入设备报告。（未开始，需先确认 VBUS 供电与 USB mux 切换）
2. 将输入转换为统一 controller state，并按目标型号编码 NS2 输入报告。（已完成，`firmware/main/ns2/`）
3. 接入 ESP32 BLE peripheral，完成广播、GATT、输入通知和主机输出命令。（代码完成，`firmware/main/ble/` + `firmware/main/dp/`，合成源静置、按键由调试页注入，实机互操作待验证）
4. 实现配对、回连、唤醒、凭证存储和震动输出；字段与流程参照 [controller.md](controller.md)，每一步都需要真实设备验证。（配对/回连/NVS 凭证代码完成，唤醒广播顺延；震动解析记录，M5 转发 USB）
5. 将连接/配对/电池等低频状态接入产品 bridge，供 PocketJS UI 显示和控制。（配对/连接已真实化，电池仍为占位）

USB 高频报告不应通过 PocketJS UI turn 或 JSON bridge 转发；bridge 只作为控制面，数据面应使用 ESP-IDF 原生任务和队列。

## 常见问题

### `bun not found`

项目脚本通过 Bun 执行 `ui/vendor/pocketjs/tools/pocket.ts`。安装官方 Bun 并确保它位于当前 PowerShell 的 `PATH`，再重试 `pnpm run check` 或 `pnpm run build`；如果看到缺少依赖的报错，先执行一次 `pnpm install`。

### `Cannot find module './styles.generated.ts'`

快照里的 `ui/vendor/pocketjs/framework/src/styles.generated.ts` 缺失，或它没有进入 pnpm 的依赖副本。从 Git 恢复该文件后重新执行 `pnpm install`；如果用的是外部 checkout，先在其目录里执行官方 `bun tools/build.ts` 生成这个镜像。

### 屏幕上中文显示为方框（tofu）

中文字形是否可用取决于烘焙图集。`ui/src/fonts.json` 已把 `ui/assets/fonts/NotoSansSC-Regular.otf` 声明为回退字体面，源码字符串里出现过的中文会在 `pnpm run compile` 时自动烘焙进各字号槽位。仍显示方框的常见原因：文本是运行时动态拼接、且字符从未出现在任何源码字面量里；或使用了字体不覆盖的码点（emoji 等符号没有字形，只会渲染为方框）。新增或修改文案后重新执行 `pnpm run compile`（或 `pnpm run build`）即可。

### `pocketjs_compile_app requires the PocketJS CLI in PATH`

这是官方 CMake helper 的预期错误。优先在项目根目录执行 `pnpm run build` 生成 `ui/dist/remapad-ui.pocket`；如果要使用 CMake 自动编译路径，需要把官方 `pocket` CLI 放入 ESP-IDF 构建进程的 `PATH`，并确保它能定位 PocketJS framework checkout。

### 固件日志有 package admission 错误

确认 `.pocket` 是由同一份 `firmware/pocket.host.json` 生成的，且没有手动修改 profile 的视口、tick、presentation、raster density 或 capabilities。改动 profile 后重新执行 `pnpm run build`。

### 烧录后没有屏幕画面

面板由 `drivers/panel.c` 驱动（esp_lcd 内置 ST7789，SPI2 40 MHz）。正常时序是：`firmware/main/boot_splash.c` 在面板与触摸初始化成功后自绘启动画面，背光随启动画面落屏由 `drivers/backlight.c` 点亮，随后每个启动阶段推进一次进度条；PocketJS UI 首帧提交成功后启动画面交出屏幕并释放缓冲。若画面不可见，先看串口日志：`panel init failed` 表示面板初始化失败（此时固件跳过启动画面，退回无面板渲染，帧只进 PSRAM）；有启动画面日志但屏幕黑，再检查背光（`GPIO15` 需要显式驱动，若 `backlight init failed` 会有对应日志）与面板排线；日志里没有启动画面但 UI 正常，说明是从旧镜像启动，重新烧录即可。修改面板方向/偏移配置时要对照 [hardware.md](hardware.md) 与微雪官方示例，不要凭空猜测初始化序列。

### 启动时崩溃重启，崩溃位置每次都不一样

典型现象是日志停在 `remapad_app: PSRAM free: ...` 之后，然后出现 `Interrupt wdt timeout`、堆锁卡死，或 `LoadProhibited` 且两次复位的崩溃点不同。这类“位置漂移”的崩溃通常不是空指针，而是栈溢出写穿了相邻内存。

根因在 QuickJS 的栈守卫：`pocketjs_guest` 默认把 `stack_limit` 设为 256 KB，而守卫判据是 `stack_top - stack_size`，其中 `stack_top` 取自**创建 runtime 的那个任务**（`JS_UpdateStackTop` 在本仓库和组件里都没有人调用）。如果调用它的任务栈比这个预算小，守卫永远不会触发，Vue Vapor 的 mount 递归会直接压坏隔壁的堆元数据。

因此**整套 guest 生命周期（创建、mount、eval、逐帧 turn）必须跑在同一个任务上**，并且给这个任务足够的栈。当前由 `firmware/main/pocketjs_host.c` 里的 `remapad-pjs` owner task 承担，栈放在 PSRAM。改动这块时不要只调 `stack_limit` 而不动任务栈，也不要让 turn 换到另一个任务上执行。

### 设备上看到的错误是 `TypeError: not a function`

这是错误上报路径自己失败，不是真正的故障。quickjs-ng 的 `js_std_add_helpers` 只给全局 `console` 装了 `log`，而框架 polyfill 的守卫写的是 `typeof console !== 'object'`，看到这个半成品对象就跳过补齐，于是 `console.warn` / `console.error` 从未安装。框架渲染器把所有捕获到的异常都交给 `console.error`，方法缺失时原始错误就被 `TypeError: not a function` 顶替。

`ui/src/index.tsx` 现在会在挂载前补齐缺失的 `console.warn` / `console.error`，转发到 native `console.log`（经 QuickJS `js_print` 进串口）。如果又看到这个报错，先确认那段垫片还在。诊断时还可以临时提高 `Error.stackTraceLimit`：QuickJS 默认只保留 10 层栈帧，栈溢出会被截断成看不出形态的短栈。

### 启动 mount 阶段出现 `task_wdt` 告警

从 `app_main` 到首帧就绪之间有一个约 5 到 6 秒的窗口，期间 owner task 连续占用一个核，空闲任务得不到调度，`task_wdt` 会打印 `IDLE0` 未按时喂狗的告警。`CONFIG_ESP_TASK_WDT_PANIC` 没有开启，所以这只是日志噪音，不影响运行。若后续对启动时间有要求，需要在 BSP 阶段优化 mount 耗时，而不是简单调大看门狗超时。

### 运行时反复 `task_wdt` 告警并且 UI 掉帧

先看 owner task 打印的帧统计（每 5 秒一条，`frames=` / `avg_turn_us=` / `avg_render_us=`）。`avg_turn_us + avg_render_us` 接近或超过 `1e6 / tickHz` 时，说明每帧把整个周期都吃满了，空闲任务自然喂不上狗。

已知的一个原因是 CPU 频率停留在默认的 160 MHz；`firmware/sdkconfig.defaults` 现在显式配置为 240 MHz。提高频率后仍有告警，就要从应用侧入手（减少每帧重绘区域或降低动画频率），而不是继续加栈。

### 移除 `pocketjs_runner` 后编译报 `esp_timer.h: No such file or directory`

`esp_timer` 之前是由 `pocketjs_runner` 间接引入的。改用产品 owner task 后需要在 `firmware/main/CMakeLists.txt` 的 `REQUIRES` 里显式声明 `esp_timer`。同一原则适用于任何原先依赖 runner 传递的头文件。

### `Could not open COM3, the port is busy`

多半是上一轮 `idf.py monitor` 的 python 进程没退干净，占着串口。按进程精确清理后再烧录：

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -like '*idf_monitor*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

不要按 `node.exe` 或 `python.exe` 之类的进程名批量结束，这些是多个项目共用的进程。

### 屏幕上没有出现 BLE 手柄广播

BLE 手柄外设已接入（`firmware/main/ble/`，见 [ROADMAP.md](ROADMAP.md)）：开机后设备以厂商数据广播出现（nRF Connect 可见 Company ID `0x0553`），但**主机互操作尚未实机验证**——Switch 2 能否发现、连接并完成 0x15 配对取决于协议逆向细节，验证前不要宣称支持 NS2。排查顺序：先看启动日志有无 `host synced` 与 GATT 句柄表，再确认广播载荷，最后对照 [controller.md](controller.md) 逐段核对。USB 输入源尚未接入（M5），当前合成源保持静置，按键输入仅来自调试页的注入按钮。

### `unsupported QuickJS source; review immutable-buffer patch before upgrading`

仓库内的 `firmware/components/pocketjs_guest` 已经按 Registry 实际内容修正了该校验值，出现这个报错说明组件被上游版本覆盖过。按 [patches/README.md](../patches/README.md) 重新核对并修正。

### `Missing pocketjs_idf_ui_core for esp32s3`

ESP32-S3 原生归档随组件固定在 `firmware/components/*/lib/esp32s3/`，正常构建不需要额外操作。出现这个报错说明归档或它的 build receipt 缺失：从 Git 恢复这两个文件即可。只有在登记上游更新、需要重新生成归档时才执行 `pnpm run native`（配合固定版本的 Xtensa Rust）；官方 CMake 不会自行下载或构建工具链。

### 预览页提示缺少 wasm 核心

触摸预览需要 Rust 构建的官方 wasm 核心。执行 `rustup target add wasm32-unknown-unknown` 后重试 `pnpm run dev`，脚本会在缺少 `pocketjs.wasm` 时调用官方 `tools/wasm.ts` 生成。

### 预览页可以点，但固件上触摸无效

设备端触摸由 `drivers/touch.c` 采样 CST816T 并经 owner task 的 `sample_input` 填入官方触点契约（`firmware/pocket.host.json` 已声明 `input.touch`）。触摸无效时先看启动日志有无 `touch init failed`（多为 I2C 无应答，检查地址 `0x15` 与共享总线接线）；init 失败时固件继续运行，但每帧触点为零。改过 profile 或驱动后需要重新 `pnpm run build` 与 `idf.py build`，旧包不会带新能力。

### `ui/dist` 或 `firmware/build` 出现文件

这些目录是生成目录，已被 Git 忽略。不要手动编辑其中的 JavaScript、PAK、`.pocket`、C/汇编嵌入源或生成头文件。

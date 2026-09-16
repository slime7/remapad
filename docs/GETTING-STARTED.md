# Remapad 新手开发与上手指南

本指南面向微雪 ESP32-S3-Touch-LCD-1.69 目标板，说明 UI 检查、PocketJS 包构建、ESP-IDF 编译和当前 bring-up 边界。
Remapad 的最终产品链路是 USB 输入→NS2 手柄报告→BLE 输出，并通过屏幕 UI 管理连接和配对；
协议资料见 [controller.md](controller.md)，板卡规格与引脚见 [hardware.md](hardware.md)。

## 前置环境

| 工具 | 版本/要求 | 用途 |
| :--- | :--- | :--- |
| Node.js | 18 或更高 | 运行项目脚本和已发布 CLI |
| pnpm | 当前稳定版 | 工作区依赖与任务调度 |
| Bun | PocketJS 官方要求的版本 | 执行官方 compiler、官方构建脚本和 Web 开发主机 |
| PocketJS compiler | 仓库内的 `ui/vendor/pocketjs` 快照 | 提供 `tools/pocket.ts` 与 ESP-IDF host profile 支持；npm 上发布的 0.11.0 尚不含该支持 |
| Xtensa Rust | `esp-rs/rust-build` 的 `v1.97.0.0` | 仅在升级组件、重新生成 ESP32-S3 原生归档时需要 |
| Python | 由 ESP-IDF 安装环境提供 | `idf.py`、ESP-IDF 工具链和官方 package 嵌入步骤 |
| uv | 当前稳定版 | 运行 `pc/` 下的工具（`cd pc ; uv run python remapadctl.py -p COMx`）；第三方依赖只有 `hidapi`，由 uv 按 `pc/pyproject.toml` 装进 `pc/.venv`，解释器要 3.10 或更高，uv 找不到会自己下载 |
| ESP-IDF | `>=6.0,<6.2` | PocketJS 官方 ESP-IDF 组件要求；本仓库已在 6.1 上验证 |
| 硬件 | 微雪 ESP32-S3-Touch-LCD-1.69（ESP32-S3R8） | 16 MB Flash、8 MB Octal PSRAM、240 × 280 ST7789V2 触摸屏；细节见 [hardware.md](hardware.md) |

目标 NS2 手柄型号和 BLE 天线/射频属于最终硬件范围。
两条输入路径（PC 桥接见 [pc/README.md](../pc/README.md)，手柄插板卡的 USB host 直插见「USB 手柄直插」一节）的代码都已落地，实机核对与 VBUS 供电确认待做。
不要因为 Web 预览可以交互就认为真实 BLE 链路已经可用。

板卡已知信息都记录在 [hardware.md](hardware.md)：
屏幕为 ST7789V2（240 × 280，4-wire SPI），触摸为 CST816T（I2C `0x15`），面板和触摸的具体引脚、共享 I2C 总线、背光控制脚和 USB 口约束都在那里。
固件已通过 `drivers/` 中的 panel/touch/backlight BSP 点亮屏幕并上报触点（选型见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)）；
BLE 手柄数据面已接入（[ADR 0010](adr/0010-nimble-ble-controller-stack.md)）。
协议边界见 [ADR 0011](adr/0011-controller-dataplane-module-boundary.md)，主机互操作待实机验证（进度见 [ROADMAP.md](ROADMAP.md)）；
USB 输入与 IMU/RTC 等其余外设仍待实现（电池电压采样已接入，充电状态只能按电压趋势推断，见 [hardware.md](hardware.md)）。

## 最短步骤

### 1. 安装依赖

```powershell
pnpm install
```

六个官方 ESP-IDF 组件与 ESP32-S3 原生归档已随仓库固定在 `firmware/components/`。
Web 开发主机随 `ui/node_modules/@pocketjs/framework` 一起安装，因此这一条之后就只剩构建命令。

前端检查、编译和打包使用仓库内的 `ui/vendor/pocketjs` 快照，依赖由 `pnpm install` 安装，因此不需要额外准备。
快照的来源与同步方式见 [ui/vendor/pocketjs/README.md](../ui/vendor/pocketjs/README.md)。

### 2. 准备 PocketJS ESP-IDF 依赖（升级时）

组件随仓库提供，日常开发不需要这一步。只有在登记上游 PocketJS 更新、或 ESPComponentRegistry 的 `espressif/quickjs-ng` 内容变化时，才需要重新对账：

1. 把上游 `hosts/esp-idf/components/` 的最新源码同步进本仓库的 `firmware/components/`。
2. 用固定版本的 Xtensa Rust 重新生成 ESP32-S3 原生归档：

```powershell
# 指向 esp-rs/rust-build v1.97.0.0 的 cargo，路径按本机安装位置调整
$env:POCKETJS_CARGO = "$env:USERPROFILE\.esp-rust\1.97.0.0\bin\cargo"
pnpm run native
```

3. 核对 `pocketjs_guest` 的 QuickJS 源码校验值。本仓库固定的副本已经使用 Registry 当前的哈希；
   若上游换用新的 `espressif/quickjs-ng`，按 [patches/README.md](../patches/README.md) 重新记录，并同步 `build-receipt.json` 中的编译器信息。

`pnpm run native` 需要能访问 PocketJS 源码（`POCKETJS_ROOT` 或仓库同级 `../pocketjs`），产物直接写入 `firmware/components/*/lib/esp32s3/`；
`idf.py build` 本身不需要 Rust。

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

`scripts/pocketjs.mjs` 按 `POCKETJS_ROOT`、`ui/vendor/pocketjs`、仓库同级 `../pocketjs` 的顺序定位包含 `--host-profile` 的官方脚本；
默认命中仓库内的快照。它只负责路径与参数转发、建立快照的依赖链接，不实现 compiler，也不改变 package 格式。

快照同时携带编译器生成的 `framework/src/styles.generated.ts`（class 字面量到 styleId 的映射）。它必须提交：
官方 CLI 的类型检查跑在编译器写入该文件之前，而 `pnpm install` 之后新写入的快照文件不会进入 pnpm 的依赖副本，缺少它时连 `pnpm run check` 都会以 TS2307 失败。
每次 `pnpm run build` 会按当前 `ui/src` 重新生成该文件，出现差异时正常提交即可。

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

该命令先用官方 `compile` 把 bundle 与 PAK 写入 `ui/dist/`，再启动项目内的触摸预览页。
打开 [http://127.0.0.1:8130](http://127.0.0.1:8130) 可以看到 240 × 280 屏幕、触摸输入和运行读数。
同一命令还会用快照内的官方 `hosts/web/serve.ts` 拉起官方 DevTools 服务器。
预览页的「DevTools 面板」按钮会打开 [http://127.0.0.1:8131/devtools](http://127.0.0.1:8131/devtools)：
组件树与屏幕高亮、暂停/单步、console 镜像与 REPL、输入磁带导出/重放/时序回退、截图，全部由官方面板与 hub 承载，预览页只负责按官方 `engine.js` 的设备协议接入 `/ws`。

注意浏览器会把后台标签页的 `requestAnimationFrame` 停掉：聚焦面板标签页时预览页（后台）帧循环暂停，面板数据随之静止。请使用并排窗口或双显示器让预览页保持可见，或者只在暂停调试时聚焦面板。

交互方式按设备的触摸屏设计：在屏幕上按下、拖动、抬起即可，没有虚拟按键和键盘映射。
预览页把指针事件转换为官方触摸帧契约（`frame(buttons, analog, touches, hits)`），触点坐标使用逻辑像素，命中事实在按下瞬间查询一次，因此点击、拖动和手势与真机走同一套判定逻辑。

预览页是项目自己的页面（`ui/preview/index.html` + `scripts/preview-server.mjs`），渲染核心和触摸语义来自官方 `@pocketjs/framework` 的浏览器运行时；
官方 playground 面向 PSP 按键，本项目不使用它。首次运行需要 Rust 的 `wasm32-unknown-unknown` target 构建 `pocketjs.wasm`，之后直接复用。

面板读数中的「触摸帧」是含触点的帧数，「命中节点」是按下时的命中结果，可用于确认触摸链路是否正常。

面板的「屏幕尺寸」开关在原始尺寸（1×）与放大尺寸（2×）之间切换，也可以按屏幕实际像素观察绘制结果。它只改变浏览器里的显示大小，触点坐标按画布实际尺寸换算，切换尺寸不影响触摸判定；选择记录在浏览器本地，刷新后保留。

### 6. 编译 ESP-IDF 固件

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

`firmware/components/` 中的官方组件由 ESP-IDF 自动发现，`firmware/main/CMakeLists.txt` 按顺序接入包：

1. 如果 `ui/dist/remapad-ui.pocket` 存在，使用官方 `pocketjs_embed_package`。
2. 否则使用官方 `pocketjs_compile_app`，让 CMake 调用 PocketJS CLI 生成 build 目录内的包。

建议先运行 `pnpm run build`，再运行 `idf.py build`。预构建路径只需要 Python 执行官方嵌入脚本，不需要 Bun；编译路径则需要可被 CMake 找到的官方 `pocket` CLI 和 Bun。

嵌入过程把 `ui/dist/remapad-ui.pocket` 登记为 CMake 依赖。
所以改完 `ui/src` 之后重新执行 `pnpm run build` 与 `idf.py build`，固件会自动重新嵌入新包，不需要删除 `firmware/build/`。

### 7. 烧录与监视

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

应用镜像已经内嵌 `.pocket` 包，烧完这三个文件就是完整的设备固件。

日常迭代只改应用层（`ui/` 产物或 `firmware/main/`）时，bootloader 和分区表没有变化，可以只重写 `0x10000` 处的应用分区，比整片烧录快，对 Flash 的擦写也更少：

```powershell
idf.py -p COM3 app-flash monitor
```

改动 bootloader、分区表或 `sdkconfig` 后仍需完整 `flash`。esptool 的等价操作是对 `0x10000` 单独 `write-flash`。

分区表自 ADR 0009 起为终局布局（`ota_0`/`ota_1` 双应用分区 + `storage` 通用存储区，`ota_0` 继承原 factory 的 `0x10000`）。烧录时注意：

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

+## 自动化测试

两套测试都在开发机上跑，不需要真板；细节与回归规则见 [TESTING.md](TESTING.md)。

```powershell
pnpm run test:e2e         # UI 端到端：Playwright 驱动触摸预览页，断言行为与像素
pnpm run test:firmware    # 固件主机端：把纯逻辑模块编译成本机可执行文件并运行
```

`test:e2e` 会自己按 `pnpm run dev` 的方式编译产物并拉起预览服务器（8130），本地已有 dev 会话时直接复用；
加 `--headed`（根脚本是 `pnpm run test:e2e:headed`）可以看到点击过程。`test:firmware` 会自动探测本机编译器（MSVC / clang / gcc，可用 `CC` 指定），几秒钟出结果。

改 UI 的 bug 时先在 `ui/tests/e2e/` 写一条能复现的用例，改完让用例转绿；改固件里与硬件无关的逻辑（NS2 编码、序列号、命令帧、像素回调、输入源合成）同理，先补 `firmware/test/` 下的用例。

## 串口 CLI 与 PWR 按键


固件在唯一的 Type-C（USB-Serial/JTAG，主控制台）上提供行命令 CLI，验收时可以不碰屏幕。与 `idf.py monitor` 共用端口，二者不要同时打开。
项目自带 [pc/remapadctl.py](../pc/remapadctl.py)（桥接转发、命令行、实机截图与 OTA 都在同一个进程里），串口与帧编解码实现在 [pc/link.py](../pc/link.py)。
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
uv run python remapadctl.py -p COM3 link            # 两只手柄的地址、连接间隔（itvl，4 = 5ms）、特性启用（feat）与上报计数
uv run python remapadctl.py -p COM3 headset auto    # 耳机状态字节：auto 按输入设备派生，也可钉住 0xNN 做主机侧 A/B
uv run python remapadctl.py -p COM3 shot            # 请求一次实机截图（PC 侧拼齐后存 PNG）
uv run python remapadctl.py -p COM3 fwver 2.0.0     # 改写上报给主机的手柄固件版本（0x10 查询与出厂块共用；不带参数看当前值）
uv run python remapadctl.py -p COM3 version         # 运行镜像版本与分区、是否待验证
uv run python remapadctl.py -p COM3 rollback        # 回滚到上一个可用镜像（仅待验证状态）
uv run python remapadctl.py -p COM3 backlight 60    # 背光并持久化
uv run python remapadctl.py -p COM3 screen off      # 息屏（on 恢复）
uv run python remapadctl.py -p COM3 mode host       # 切到 host：COM 口消失，日志与 CLI 改走 UART0（otg 仍被拒绝）
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
uv run python remapadctl.py -p COM3 poweroff        # 关机（释放电源锁存，仅电池供电有效）
uv run python remapadctl.py -p COM3 reboot          # 软重启回 COM 模式
uv run python remapadctl.py -p COM3 --log --seconds 20         # 只读设备日志 20 秒
uv run python remapadctl.py -p COM3 --log --reset --seconds 25  # 先复位再抓完整启动日志
uv run python remapadctl.py -p COM3 --shot --out shots\ui.png   # 抓实机截图并指定输出路径
```

`key` 的键名为 `a b x y plus minus home capture c l r zl zr ls rs up down left right gl gr ui`。
默认保持 250 ms（`ui` 为 500 ms，盖过组合键 300 ms 的翻转阈值），最长 60000 ms；注入叠加在输入源之上。
`stick` 设定的一侧摇杆持续生效、未设定的一侧沿用输入源，因此可以分别推左摇杆与右摇杆做对照。`link` 打印当前身份一行：
对外广播地址、连接句柄、会话状态（idle / advertising / wait-pair / normal）、报告格式、已开启的通知通道、已发送报告数与凭证条数，配对与回连过程可以直接在串口上对账。

`ui` 与 `ui on` / `ui off` 对应手柄操控屏幕模式（[ADR 0028](adr/0028-pad-combo-captures-screen.md)）：
`key ui` 注入的就是 L1+R1+L3+R3 组合键（保持 500 ms，盖过 300 ms 的翻转阈值），进模式后方向键移动焦点、圆圈键等价于点按屏幕，再按一次组合键退出；
`ui on` / `ui off` 直接置位，不经过组合键判定，用来单独确认模式的开关与退出恢复。方向键在模式里分两个轴（见 [ADR 0029](adr/0029-pad-ui-axis-split.md)）：
上下在页面内容里走，走到最后一项再按下会让页面继续往下滚到页底；
左右只在悬浮底栏两项之间走，实机上单独按住 L1 / R1 与按左 / 右等价（注入用 `key l` / `key r`；组合键以 L1 + R1 起手，四键同按与两肩键同按都不发方向）。
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
有链路或正在广播时停止广播并断开（设备平时静默，见 [ADR 0038](adr/0038-user-initiated-connection-window.md)）。USB 角色切换只在模式页与串口 `mode` 里做。
长按到 3 秒时蜂鸣器（GPIO42，`drivers/buzzer.c`；LEDC 定时器与通道与背光分离，两者占空比互不覆盖）短鸣一声提示可以松开；按住超过 6 秒不产生软件事件。
SYS_EN（GPIO41）电源保持脚由固件在 `app_main` 入口最先拉高锁存：电池供电时松开 PWR 键后系统继续工作，复位窗口也不会掉电；USB 供电下锁存被旁路，拉高无副作用。
软件关机走系统页「关机」按钮（bridge 的 `powerOff` 命令，串口对应 `poweroff`）：电池供电下释放锁存即断电，USB 供电下锁存被旁路、关不掉，固件重新锁存后界面提示「USB 供电下无法关机，请拔线后再试」。

用户设置（背光亮度、手柄四段配色、上报固件版本）持久化在 NVS（`firmware/main/config/app_config.c`），重启后恢复；息屏状态与 USB 连接模式不跨重启保留（USB 角色开机恒为串口）。
PC 手柄经桥接程序进入设备这条路径已落地：设备侧见 `firmware/main/input/`，PC 侧见 [pc/README.md](../pc/README.md)；
手柄插在板卡上的 USB host 直插也已落地（`firmware/main/usb/`）。
实机核对清单见 [ROADMAP.md](ROADMAP.md) M5 与 [usb-input-plan.md](usb-input-plan.md)。

## 固件 OTA 升级

整包应用镜像（固件 + 内嵌 `.pocket`）可以在不接线烧录的情况下升级：PC 端把镜像经 USB-Serial/JTAG 推给设备，设备写进当前未运行的应用分区，`esp_ota_end` 校验通过后切换启动分区并重启。
选型与协议见 [ADR 0022](adr/0022-ota-over-bridge-frames-with-rollback.md)。
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

- 升级前先跑 `pnpm run build` 与 `idf.py build`，镜像就是 `firmware/build/remapad_firmware.bin`；
  设备只接受项目名为 `remapad_firmware` 的 ESP32-S3 应用镜像，尺寸上限是应用分区容量 4 MB。
 升级由持有 COM 口的那个进程执行：`remapadctl.py --upgrade` 自己就是持有者，桥接转发与命令行在同一会话里照常；先退出 `idf.py monitor` 等其它占用进程，设备必须处于 COM 模式（host 模式或 OTG 切换后 COM 口不存在）。
  升级与设备当前是否连着 NS2 主机无关，重启后按凭证回连。
- 校验通过后设备自动重启，首次启动处于「待验证」状态：UI 首帧成功且稳定运行满 30 秒才标记为有效，在此之前断电或重启会自动回退到升级前的镜像，此时 `version` 显示 `image=pending-verify`。
- 升级中断（PC 退出、拔线、断电）不影响启动：`otadata` 在成功前不动，设备仍从旧镜像启动，残留在另一个分区的半镜像会在下次升级时重新擦写。
- 从 `ota_1` 启动之后，开发期 `app-flash` 固定写 `0x10000`（`ota_0`）未必是当前启动分区。
  继续开发前先执行 `idf.py -p COM3 erase-otadata`（引导器随后回退到 `ota_0`）。

## 关键文件

- [ui/pocket.json](../ui/pocket.json)：应用清单和应用侧 capability。
- [ui/src/fonts.json](../ui/src/fonts.json)：应用目录的回退字体清单；中文字体烘焙见 [ui/assets/fonts/](../ui/assets/fonts)。
- [firmware/pocket.host.json](../firmware/pocket.host.json)：ESP32-S3 host profile。
- [firmware/components/](../firmware/components)：固定的官方 ESP-IDF 组件与 ESP32-S3 原生归档。
- [firmware/main/CMakeLists.txt](../firmware/main/CMakeLists.txt)：官方 package embed/compile 接入。
- [firmware/main/pocketjs_host.c](../firmware/main/pocketjs_host.c)：
  package、guest、binding、renderer 生命周期与 `remapad-pjs` owner task。
- [firmware/main/boot_splash.c](../firmware/main/boot_splash.c)：UI 就绪前的固件自绘启动画面（主题底色 + 手柄标记 + 阶段进度条），同时在启动画面落屏后提前点亮背光。
- [firmware/main/config/app_config.c](../firmware/main/config/app_config.c)：用户设置 NVS 持久化（亮度 / 连接模式 / 手柄身份）。
- [firmware/main/console/cli.c](../firmware/main/console/cli.c)：串口行命令 CLI（USB-Serial/JTAG）。
- [firmware/main/drivers/pwr_key.c](../firmware/main/drivers/pwr_key.c)：PWR 按键采样（短按息屏、长按是连接键）。
- [firmware/main/dp/dp_source.c](../firmware/main/dp/dp_source.c)：数据面输入源抽象（注册制；桥接源在 `input/`，USB host 源在 `usb/`）。
- [firmware/main/usb/](../firmware/main/usb)：USB host 直插（枚举与 HID 收发、输入源、运行时角色切换）与主机反馈写回。
- [firmware/main/pad/feedback.c](../firmware/main/pad/feedback.c)：反馈编码（按设备布局行把震动 / 玩家灯 / 触觉采样编码成该手柄的输出报告）。
- [firmware/main/input/input_link.c](../firmware/main/input/input_link.c)：桥接链路的设备侧（USB-Serial/JTAG 唯一读取者、桥接帧与 CLI 文本分流）。
- [firmware/main/pad/pad_device.c](../firmware/main/pad/pad_device.c)：私有手柄格式与解析（按键位置映射、轴归一、死区）；
  家族布局表按系列拆在 [firmware/main/pad/layouts/](../firmware/main/pad/layouts)。
  契约与注册表是 [layout.h](../firmware/main/pad/layout.h) / [layout.c](../firmware/main/pad/layout.c)。
- 目标编码接口 [target.c](../firmware/main/target/target.c) 与 NS2 输出封装 [ns2/](../firmware/main/target/ns2)：
  涵盖按键构建报告、结构化反馈、电池与 amiibo 预置。
- [pc/remapadctl.py](../pc/remapadctl.py) 与 [pc/link.py](../pc/link.py)：
  PC 侧单工具（hidapi 读手柄 → 桥接帧、串口命令行、实机截图与 OTA 在同一个进程里；`--dump` 核对家族表偏移；依赖与运行方式见 [pc/README.md](../pc/README.md)）。
- [firmware/main/ota/](../firmware/main/ota)：OTA 升级会话与协议（分区回写、窗口流控、回滚健康门槛），PC 端入口是 `remapadctl.py --upgrade`。
- [firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)：Flash/PSRAM、CPU 频率、FreeRTOS 与主控制台（USJ）预设。
- [firmware/partitions.csv](../firmware/partitions.csv)：NVS、PHY、OTA 双应用分区和通用存储区（storage）的终局布局（ADR 0009）。
- [scripts/pocketjs.mjs](../scripts/pocketjs.mjs)：编译器、触摸预览和原生归档脚本的统一入口。
- [agent-temp/](../agent-temp)：代理与调试的临时文件目录（脚本、抓包输出、截图与日志；内容不进版本库，约定见 [AGENTS.md](../AGENTS.md)）。
- [ui/preview/index.html](../ui/preview/index.html)：触摸屏预览页与触摸帧契约实现。
- [ui/src/App.tsx](../ui/src/App.tsx)：
  首屏前一次挂完七个页面的页面调度（切页由页面根节点自行切换 `hidden`，新增页面直接写在 JSX 里，见 [ADR 0016](adr/0016-mount-all-pages-before-first-frame.md)）。
- [patches/README.md](../patches/README.md)：与上游组件的差异记录、QuickJS 校验值核对与升级步骤。
- [docs/controller.md](controller.md)：NS2 手柄 USB/BLE、广播、GATT、HID 报告和配对规范。
- [docs/hardware.md](hardware.md)：目标板卡的 SoC/存储、屏幕、触摸、外设、GPIO 分配和板级注意事项。
- [docs/usb-input-plan.md](usb-input-plan.md)：USB host 直插的方案与实机核对清单（代码已落地，桥接路径见 [pc/README.md](../pc/README.md)）。

### USB 手柄直插（host 模式）

手柄插在板卡 Type-C 上时设备做 USB 主机：`firmware/main/usb/` 装 host 栈、按报告描述符挑手柄用途的 HID 接口（跳过厂商与音频接口）、收 IN 报告后按 VID/PID 走同一份家族布局表。
真 Switch 2 手柄的报文体原样转发给 NS2 主机（真电量与真陀螺仪直达），其余家族解析成私有格式后重新编码；主机下发的震动与玩家灯按布局行编码写回手柄的 OUT 端点。

切换入口有两个：模式页「手柄」卡片、串口 `mode host`；角色只在本次运行有效、不写 NVS。
切过去之后在 UART0 上敲 `pad` 看识别结果与是否透传，敲 `usb` 看 host 栈状态与收发计数。

- host 模式下 PC 上不再有 COM 口：串口 CLI、桥接程序与 OTA 都用不了，日志与 CLI 改走 UART0（GPIO43/44 扩展焊盘接 USB-UART 适配器，115200）。
- 回到串口有两条路：在 UART0 上敲 `mode device`（或从模式页切回「串口」），或者复位——复用开关复位默认回 USB-Serial/JTAG，COM 口天然回来，烧录不受影响。
- 识别结果看 `pad`（家族、VID:PID、命中的布局行、兜底标记、是否透传）与 `usb`（枚举到的设备、报告与写回计数）；未登记的 VID/PID 回落 Xbox 有线布局并打兜底标记。
- 门禁：host 模式要给插入的手柄供 VBUS 5V，供电路径还没确认（[hardware.md](hardware.md) 挂起项）；确认前手柄能否枚举只能在实机验证。

## 最终产品数据面（当前规划）

后续固件工作按以下顺序拆分（进度跟踪见 [ROADMAP.md](ROADMAP.md)，BLE 链路先行、USB 输入殿后）：

1. 接入 ESP-IDF USB host，接收并解析输入设备报告。（代码完成：`firmware/main/usb/` 枚举 HID 手柄、按 VID/PID 走同一份家族表，实机核对与 VBUS 供电确认待做）
2. 将输入转换为统一 controller state，并按目标型号编码 NS2 输入报告。
   （已完成，按 `firmware/main/input/` → `pad/` → `target/` 三段划分，见 [ADR 0021](adr/0021-input-path-three-stage-layering.md)）
3. PC 手柄经桥接程序与串口帧进入设备，映射与编码走同一套 `pad/` + `target/`。（设备侧与 PC 侧代码已完成，实机验收与家族表抓包核对待做）
3. 接入 ESP32 BLE peripheral，完成广播、GATT、输入通知和主机输出命令。
   （代码完成，`firmware/main/ble/` + `firmware/main/dp/`，合成源静置、按键由调试页注入，实机互操作待验证）
4. 实现配对、回连、唤醒、凭证存储和震动输出；字段与流程参照 [controller.md](controller.md)，每一步都需要真实设备验证。（配对/回连/NVS 凭证代码完成，唤醒广播顺延；震动解析记录，M5 转发 USB）
5. 将连接/配对/电池等低频状态接入产品 bridge，供 PocketJS UI 显示和控制。（配对/连接与电池电量已真实化；充电状态为电压趋势推断值）

USB 高频报告不应通过 PocketJS UI turn 或 JSON bridge 转发；bridge 只作为控制面，数据面应使用 ESP-IDF 原生任务和队列。

## 常见问题

### `bun not found`

项目脚本通过 Bun 执行 `ui/vendor/pocketjs/tools/pocket.ts`。
安装官方 Bun 并确保它位于当前 PowerShell 的 `PATH`，再重试 `pnpm run check` 或 `pnpm run build`；如果看到缺少依赖的报错，先执行一次 `pnpm install`。

### `Cannot find module './styles.generated.ts'`

快照里的 `ui/vendor/pocketjs/framework/src/styles.generated.ts` 缺失，或它没有进入 pnpm 的依赖副本。从 Git 恢复该文件后重新执行 `pnpm install`；
如果用的是外部 checkout，先在其目录里执行官方 `bun tools/build.ts` 生成这个镜像。

### 屏幕上中文显示为方框（tofu）

中文字形是否可用取决于烘焙图集。
`ui/src/fonts.json` 已把 `ui/assets/fonts/NotoSansSC-Regular.otf` 声明为回退字体面，源码字符串里出现过的中文会在 `pnpm run compile` 时自动烘焙进各字号槽位。
仍显示方框的常见原因：文本是运行时动态拼接、且字符从未出现在任何源码字面量里；或使用了字体不覆盖的码点（emoji 等符号没有字形，只会渲染为方框）。
新增或修改文案后重新执行 `pnpm run compile`（或 `pnpm run build`）即可。

### `pocketjs_compile_app requires the PocketJS CLI in PATH`

这是官方 CMake helper 的预期错误。优先在项目根目录执行 `pnpm run build` 生成 `ui/dist/remapad-ui.pocket`；
如果要使用 CMake 自动编译路径，需要把官方 `pocket` CLI 放入 ESP-IDF 构建进程的 `PATH`，并确保它能定位 PocketJS framework checkout。

### `idf.py build` 秒退且没有编译输出

现象是打印几行环境提示后出现下面这条错误，紧接一行 `Executing action: all (aliases: build)` 就结束，退出码 0、`firmware/build/` 里的产物时间戳不变：

```text
'<venv>\python.exe' is currently active in the environment while the project was configured with '<venv>\python.exe'
```
这不是构建成功：`firmware/build/CMakeCache.txt` 记录了配置工程时用的 Python 解释器（`rg -n '^PYTHON' firmware/build/CMakeCache.txt`）。
换到另一套 IDF 环境后 `idf.py` 只做校验就返回，一步都不编译。按提示里的两条路径切回配置工程时用的那套环境即可；
要换环境长期使用则在当前环境里 `idf.py fullclean` 后重新配置（见「6. 编译 ESP-IDF 固件」）。想确认工程到底有没有活干，可以先试运行一次 ninja，它只列步骤、不改文件：

```powershell
ninja -C firmware\build -n
```

### 固件日志有 package admission 错误

确认 `.pocket` 是由同一份 `firmware/pocket.host.json` 生成的，且没有手动修改 profile 的视口、tick、presentation、raster density 或 capabilities。
改动 profile 后重新执行 `pnpm run build`。

### 烧录后没有屏幕画面

面板由 `drivers/panel.c` 驱动（esp_lcd 内置 ST7789，SPI2 80 MHz，见 [ARCHITECTURE.md](ARCHITECTURE.md) 的显示通路预算）。正常时序是：
`firmware/main/boot_splash.c` 在面板与触摸初始化成功后自绘启动画面，背光随启动画面落屏由 `drivers/backlight.c` 点亮，随后每个启动阶段推进一次进度条；
PocketJS UI 首帧提交成功后启动画面交出屏幕并释放缓冲。若画面不可见，先看串口日志：`panel init failed` 表示面板初始化失败（此时固件跳过启动画面，退回无面板渲染，帧只进 PSRAM）；
有启动画面日志但屏幕黑，再检查背光（`GPIO15` 需要显式驱动，若 `backlight init failed` 会有对应日志）与面板排线；日志里没有启动画面但 UI 正常，说明是从旧镜像启动，重新烧录即可。
修改面板方向/偏移配置时要对照 [hardware.md](hardware.md) 与微雪官方示例，不要凭空猜测初始化序列。

### 启动时崩溃重启，崩溃位置每次都不一样

典型现象是日志停在 `remapad_app: PSRAM free: ...` 之后，然后出现 `Interrupt wdt timeout`、堆锁卡死，或 `LoadProhibited` 且两次复位的崩溃点不同。
这类“位置漂移”的崩溃通常不是空指针，而是栈溢出写穿了相邻内存。

根因在 QuickJS 的栈守卫：`pocketjs_guest` 默认把 `stack_limit` 设为 256 KB，而守卫判据是 `stack_top - stack_size`。
其中 `stack_top` 取自**创建 runtime 的那个任务**（`JS_UpdateStackTop` 在本仓库和组件里都没有人调用）。
如果调用它的任务栈比这个预算小，守卫永远不会触发，Vue Vapor 的 mount 递归会直接压坏隔壁的堆元数据。

因此**整套 guest 生命周期（创建、mount、eval、逐帧 turn）必须跑在同一个任务上**，并且给这个任务足够的栈。
当前由 `firmware/main/pocketjs_host.c` 里的 `remapad-pjs` owner task 承担，栈放在 PSRAM。
改动这块时不要只调 `stack_limit` 而不动任务栈，也不要让 turn 换到另一个任务上执行。

### 设备上看到的错误是 `TypeError: not a function`

这是错误上报路径自己失败，不是真正的故障。quickjs-ng 的 `js_std_add_helpers` 只给全局 `console` 装了 `log`。
而框架 polyfill 的守卫写的是 `typeof console !== 'object'`，看到这个半成品对象就跳过补齐，于是 `console.warn` / `console.error` 从未安装。
框架渲染器把所有捕获到的异常都交给 `console.error`，方法缺失时原始错误就被 `TypeError: not a function` 顶替。

`ui/src/index.tsx` 现在会在挂载前补齐缺失的 `console.warn` / `console.error`，转发到 native `console.log`（经 QuickJS `js_print` 进串口）。
如果又看到这个报错，先确认那段垫片还在。诊断时还可以临时提高 `Error.stackTraceLimit`：QuickJS 默认只保留 10 层栈帧，栈溢出会被截断成看不出形态的短栈。

### 启动 guest eval 阶段出现 `task_wdt` 告警

从 `app_main` 到首帧就绪之间有一个十几秒的窗口（当前构建实测：启动画面约 1.6 秒落屏，约 18 秒首帧就绪，背光随启动画面点亮）。
其中 `guest_eval` 占约 16 秒（阶段权重表按实测填写），期间 owner task 连续占用一个核，空闲任务得不到调度，`task_wdt` 会打印 `IDLE0` 未按时喂狗的告警。
`CONFIG_ESP_TASK_WDT_PANIC` 没有开启，所以这只是日志噪音，不影响运行。若后续对启动时间有要求，需要在 BSP 阶段优化 guest eval 耗时（编译与执行整包 JS），而不是简单调大看门狗超时。

### 运行时反复 `task_wdt` 告警并且 UI 掉帧

先看 owner task 打印的帧统计（每 5 秒一条，`frames=` / `avg_turn_us=` / `avg_render_us=`）。
`avg_turn_us + avg_render_us` 接近或超过 `1e6 / tickHz` 时，说明每帧把整个周期都吃满了，空闲任务自然喂不上狗。

已知的一个原因是 CPU 频率停留在默认的 160 MHz；`firmware/sdkconfig.defaults` 现在显式配置为 240 MHz。提高频率后仍有告警，就要从应用侧入手（减少每帧重绘区域或降低动画频率），而不是继续加栈。

### 移除 `pocketjs_runner` 后编译报 `esp_timer.h: No such file or directory`

`esp_timer` 之前是由 `pocketjs_runner` 间接引入的。
改用产品 owner task 后需要在 `firmware/main/CMakeLists.txt` 的 `REQUIRES` 里显式声明 `esp_timer`。同一原则适用于任何原先依赖 runner 传递的头文件。

### `Could not open COM3, the port is busy`

多半是上一轮 `idf.py monitor` 的 python 进程没退干净，占着串口。按进程精确清理后再烧录：

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.CommandLine -like '*idf_monitor*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

不要按 `node.exe` 或 `python.exe` 之类的进程名批量结束，这些是多个项目共用的进程。

### 屏幕上没有出现 BLE 手柄广播

BLE 手柄外设已接入（`firmware/main/ble/`，见 [ROADMAP.md](ROADMAP.md)）：
开机后设备以厂商数据广播出现（nRF Connect 可见 Company ID `0x0553`），但**主机互操作尚未实机验证**——Switch 2 能否发现、连接并完成 0x15 配对取决于协议逆向细节，验证前不要宣称支持 NS2。
排查顺序：先看启动日志有无 `host synced` 与 GATT 句柄表，再确认广播载荷，最后对照 [controller.md](controller.md) 逐段核对。
USB 输入源尚未接入（M5），当前合成源保持静置，按键输入仅来自调试页的注入按钮。

### `unsupported QuickJS source; review immutable-buffer patch before upgrading`

仓库内的 `firmware/components/pocketjs_guest` 已经按 Registry 实际内容修正了该校验值，出现这个报错说明组件被上游版本覆盖过。
按 [patches/README.md](../patches/README.md) 重新核对并修正。

### `Missing pocketjs_idf_ui_core for esp32s3`

ESP32-S3 原生归档随组件固定在 `firmware/components/*/lib/esp32s3/`，正常构建不需要额外操作。出现这个报错说明归档或它的 build receipt 缺失：从 Git 恢复这两个文件即可。
只有在登记上游更新、需要重新生成归档时才执行 `pnpm run native`（配合固定版本的 Xtensa Rust）；官方 CMake 不会自行下载或构建工具链。

### 预览页提示缺少 wasm 核心

触摸预览需要 Rust 构建的官方 wasm 核心。
执行 `rustup target add wasm32-unknown-unknown` 后重试 `pnpm run dev`，脚本会在缺少 `pocketjs.wasm` 时调用官方 `tools/wasm.ts` 生成。

### 预览页可以点，但固件上触摸无效

设备端触摸由 `drivers/touch.c` 采样 CST816T。
触点经 owner task 的 `sample_input` 填入官方触点契约（`firmware/pocket.host.json` 已声明 `input.touch`）。
触摸无效时先看启动日志有无 `touch init failed`（多为 I2C 无应答，检查地址 `0x15` 与共享总线接线）；init 失败时固件继续运行，但每帧触点为零。
改过 profile 或驱动后需要重新 `pnpm run build` 与 `idf.py build`，旧包不会带新能力。

### `ui/dist` 或 `firmware/build` 出现文件

这些目录是生成目录，已被 Git 忽略。不要手动编辑其中的 JavaScript、PAK、`.pocket`、C/汇编嵌入源或生成头文件。

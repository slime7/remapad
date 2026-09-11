# Remapad 新手开发与上手指南

本指南面向 ESP32-S3 N16R8 目标板，说明 UI 检查、PocketJS 包构建、ESP-IDF 编译和当前 bring-up 边界。Remapad 的最终产品链路是 USB 输入→NS2 手柄报告→BLE 输出，并通过屏幕 UI 管理连接和配对；协议资料见 [controller.md](controller.md)。

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
| 硬件 | ESP32-S3-WROOM-1 N16R8 | 16 MB Flash、8 MB Octal PSRAM |

USB 输入设备、目标 NS2 手柄型号、BLE 天线/射频和屏幕控制器也属于最终硬件范围，但当前仓库尚未完成这些产品 BSP。不要因为 Web 预览可以交互就认为真实 USB 或 BLE 链路已经可用。

设备屏幕是触摸屏：实际屏幕控制器、触摸芯片、引脚和串口端口需要根据开发板资料配置；仓库当前只确定 240×280 RGB565 逻辑视口，没有假定通用 ST7789 引脚表，也没有假定具体触摸控制器。

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

该命令先用官方 `compile` 把 bundle 与 PAK 写入 `ui/dist/`，再启动项目内的触摸预览页。打开 [http://127.0.0.1:8130](http://127.0.0.1:8130) 可以看到 240 × 280 屏幕、触摸输入和运行读数。

交互方式按设备的触摸屏设计：在屏幕上按下、拖动、抬起即可，没有虚拟按键和键盘映射。预览页把指针事件转换为官方触摸帧契约（`frame(buttons, analog, touches, hits)`），触点坐标使用逻辑像素，命中事实在按下瞬间查询一次，因此点击、拖动和手势与真机走同一套判定逻辑。

预览页是项目自己的页面（`ui/preview/index.html` + `scripts/preview-server.mjs`），渲染核心和触摸语义来自官方 `@pocketjs/framework` 的浏览器运行时；官方 playground 面向 PSP 按键，本项目不使用它。首次运行需要 Rust 的 `wasm32-unknown-unknown` target 构建 `pocketjs.wasm`，之后直接复用。

面板读数中的「触摸帧」是含触点的帧数，「命中节点」是按下时的命中结果，可用于确认触摸链路是否正常。

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

把 `COM3` 替换为实际端口。若开发板没有自动进入下载模式，按板卡说明操作 BOOT/EN。串口监视器使用 `Ctrl + ]` 退出。

`idf.py build` 在 `firmware/build/` 下生成三个可烧录文件，偏移与 `firmware/build/flash_project_args` 一致：

| 文件 | 烧录偏移 |
| :--- | :--- |
| `build/bootloader/bootloader.bin` | `0x0` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/remapad_firmware.bin` | `0x10000` |

应用镜像已经内嵌 `.pocket` 包，烧完这三个文件就是完整的设备固件。

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

## 关键文件

- [ui/pocket.json](../ui/pocket.json)：应用清单和应用侧 capability。
- [firmware/pocket.host.json](../firmware/pocket.host.json)：ESP32-S3 host profile。
- [firmware/components/](../firmware/components)：固定的官方 ESP-IDF 组件与 ESP32-S3 原生归档。
- [firmware/main/CMakeLists.txt](../firmware/main/CMakeLists.txt)：官方 package embed/compile 接入。
- [firmware/main/pocketjs_host.c](../firmware/main/pocketjs_host.c)：package、guest、binding、renderer、runner 生命周期。
- [firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)：N16R8 Flash/PSRAM 和 FreeRTOS 预设。
- [firmware/partitions.csv](../firmware/partitions.csv)：NVS、PHY 和 4 MB factory 分区。
- [scripts/pocketjs.mjs](../scripts/pocketjs.mjs)：编译器、触摸预览和原生归档脚本的统一入口。
- [ui/preview/index.html](../ui/preview/index.html)：触摸屏预览页与触摸帧契约实现。
- [patches/README.md](../patches/README.md)：与上游组件的差异记录、QuickJS 校验值核对与升级步骤。
- [docs/controller.md](controller.md)：NS2 手柄 USB/BLE、广播、GATT、HID 报告和配对规范。

## 最终产品数据面（当前规划）

后续固件工作按以下顺序拆分：

1. 接入 ESP-IDF USB host，接收并解析输入设备报告。
2. 将输入转换为统一 controller state，并按目标型号编码 NS2 输入报告。
3. 接入 ESP32 BLE peripheral，完成广播、GATT、输入通知和主机输出命令。
4. 实现配对、回连、唤醒、凭证存储和震动输出；字段与流程参照 [controller.md](controller.md)，每一步都需要真实设备验证。
5. 将连接/配对/电池等低频状态接入产品 bridge，供 PocketJS UI 显示和控制。

USB 高频报告不应通过 PocketJS UI turn 或 JSON bridge 转发；bridge 只作为控制面，数据面应使用 ESP-IDF 原生任务和队列。

## 常见问题

### `bun not found`

项目脚本通过 Bun 执行 `ui/vendor/pocketjs/tools/pocket.ts`。安装官方 Bun 并确保它位于当前 PowerShell 的 `PATH`，再重试 `pnpm run check` 或 `pnpm run build`；如果看到缺少依赖的报错，先执行一次 `pnpm install`。

### `Cannot find module './styles.generated.ts'`

快照里的 `ui/vendor/pocketjs/framework/src/styles.generated.ts` 缺失，或它没有进入 pnpm 的依赖副本。从 Git 恢复该文件后重新执行 `pnpm install`；如果用的是外部 checkout，先在其目录里执行官方 `bun tools/build.ts` 生成这个镜像。

### `pocketjs_compile_app requires the PocketJS CLI in PATH`

这是官方 CMake helper 的预期错误。优先在项目根目录执行 `pnpm run build` 生成 `ui/dist/remapad-ui.pocket`；如果要使用 CMake 自动编译路径，需要把官方 `pocket` CLI 放入 ESP-IDF 构建进程的 `PATH`，并确保它能定位 PocketJS framework checkout。

### 固件日志有 package admission 错误

确认 `.pocket` 是由同一份 `firmware/pocket.host.json` 生成的，且没有手动修改 profile 的视口、tick、presentation、raster density 或 capabilities。改动 profile 后重新执行 `pnpm run build`。

### 烧录后没有屏幕画面

当前固件只完成官方运行时和 RGB565 damage strip 的无面板 bring-up：`sample_input` 返回空输入，`after_turn` 尚未调用真实面板 DMA。需要根据开发板硬件资料补充产品 BSP，再将输入采样和 strip 传输接入回调。

### BLE 没有发现 NS2 手柄

当前固件尚未实现 USB→NS2→BLE 数据面，也没有配对广播或 GATT 服务。请先阅读 [controller.md](controller.md)，不要仅通过修改 PocketJS manifest 或 UI bridge 宣称已支持 NS2。

### `unsupported QuickJS source; review immutable-buffer patch before upgrading`

仓库内的 `firmware/components/pocketjs_guest` 已经按 Registry 实际内容修正了该校验值，出现这个报错说明组件被上游版本覆盖过。按 [patches/README.md](../patches/README.md) 重新核对并修正。

### `Missing pocketjs_idf_ui_core for esp32s3`

ESP32-S3 原生归档随组件固定在 `firmware/components/*/lib/esp32s3/`，正常构建不需要额外操作。出现这个报错说明归档或它的 build receipt 缺失：从 Git 恢复这两个文件即可。只有在登记上游更新、需要重新生成归档时才执行 `pnpm run native`（配合固定版本的 Xtensa Rust）；官方 CMake 不会自行下载或构建工具链。

### 预览页提示缺少 wasm 核心

触摸预览需要 Rust 构建的官方 wasm 核心。执行 `rustup target add wasm32-unknown-unknown` 后重试 `pnpm run dev`，脚本会在缺少 `pocketjs.wasm` 时调用官方 `tools/wasm.ts` 生成。

### 预览页可以点，但固件上触摸无效

预览页走浏览器指针事件，不需要固件参与；设备端的触摸需要产品 BSP 采样面板触摸芯片，再填入官方 runner 的 `sample_input`。`firmware/pocket.host.json` 在触摸采样就位前不声明 `input.touch`，因此固件当前不会向 UI 提供触点。

### `ui/dist` 或 `firmware/build` 出现文件

这些目录是生成目录，已被 Git 忽略。不要手动编辑其中的 JavaScript、PAK、`.pocket`、C/汇编嵌入源或生成头文件。

# Remapad 新手开发与上手指南

本指南面向 ESP32-S3 N16R8 目标板，说明 UI 检查、PocketJS 包构建、ESP-IDF 编译和当前 bring-up 边界。Remapad 的最终产品链路是 USB 输入→NS2 手柄报告→BLE 输出，并通过屏幕 UI 管理连接和配对；协议资料见 [controller.md](controller.md)。

## 前置环境

| 工具 | 版本/要求 | 用途 |
| :--- | :--- | :--- |
| Node.js | 18 或更高 | 运行项目脚本和已发布 CLI |
| pnpm | 当前稳定版 | 工作区依赖与任务调度 |
| Bun | PocketJS 官方要求的版本 | 执行官方 compiler、官方构建脚本和 Web 开发主机 |
| PocketJS checkout | 当前 `main` 或包含 ESP-IDF host profile 的版本 | 提供官方组件、compiler 和 `hosts/web` 开发主机；用 `POCKETJS_ROOT` 指向根目录，默认仓库同级的 `../pocketjs` |
| Xtensa Rust | `esp-rs/rust-build` 的 `v1.97.0.0` | 生成 ESP32-S3 原生归档；Web 预览还需要 `wasm32-unknown-unknown` target |
| Python | 由 ESP-IDF 安装环境提供 | `idf.py`、ESP-IDF 工具链和官方 package 嵌入步骤 |
| ESP-IDF | `>=6.0,<6.2` | PocketJS 官方 ESP-IDF 组件要求；本仓库已在 6.1 上验证 |
| 硬件 | ESP32-S3-WROOM-1 N16R8 | 16 MB Flash、8 MB Octal PSRAM |

USB 输入设备、目标 NS2 手柄型号、BLE 天线/射频和屏幕控制器也属于最终硬件范围，但当前仓库尚未完成这些产品 BSP。不要因为 Web 预览可以交互就认为真实 USB 或 BLE 链路已经可用。

实际屏幕控制器、引脚、触控芯片和串口端口需要根据开发板资料配置；仓库当前只确定 240×280 RGB565 逻辑视口，没有假定通用 ST7789 引脚表。

## 最短步骤

### 1. 安装依赖

```powershell
pnpm install
```

在 PocketJS 官方 checkout 中安装编译器依赖，并设置项目脚本使用的路径：

```powershell
cd C:\src\pocketjs
bun install
$env:POCKETJS_ROOT = 'C:\src\pocketjs'
cd C:\src\remapad
```

### 2. 准备 PocketJS ESP-IDF 依赖

ESP Component Registry 没有发布 `pocket-stack/pocketjs_*` 组件，而 `pocketjs_guest` 使用的 QuickJS 源码校验值与 registry 当前提供的 `espressif/quickjs-ng` 0.14.0 不一致，因此先应用本仓库的补丁：

```powershell
git -C $env:POCKETJS_ROOT apply "$PWD/patches/0001-quickjs-ng-0.14.0-source-pin.patch"
```

再用官方脚本生成 ESP32-S3 的两个原生 Rust 归档：

```powershell
$env:POCKETJS_CARGO = 'C:\Users\admin\.esp-rust\1.97.0.0\bin\cargo'
pnpm run native
```

归档写入 checkout 的 `hosts/esp-idf/components/pocketjs_ui_core/lib/esp32s3/` 和 `pocketjs_render_rgb565/lib/esp32s3/`，并附带 `build-receipt.json`。这两步只需执行一次，之后 `idf.py build` 不再需要 Rust。

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

`scripts/pocketjs.mjs` 按 `POCKETJS_ROOT`、仓库同级 `../pocketjs`、`ui/node_modules/@pocketjs/framework` 的顺序定位包含 `--host-profile` 的官方脚本；它只负责路径和参数转发，不实现 compiler，也不改变 package 格式。

官方命令的语义如下，适用于已正确安装并能定位 PocketJS framework checkout 的环境：

```powershell
$env:POCKETJS_ROOT = 'C:\src\pocketjs'
pocket build --manifest ui/pocket.json `
  --host-profile firmware/pocket.host.json `
  --project-root ui --outdir ui/dist `
  --output ui/dist/remapad-ui.pocket
```

不要将 `--target psp` 用在本项目上。`psp` 是 Sony PSP 后端的 target 名称；ESP32 使用自定义 `--host-profile`。

### 5. 官方 Web 预览

```powershell
pnpm run dev
```

该命令先用官方 `compile` 把 bundle 写入 checkout 的 `dist/`，再启动官方 `hosts/web` 开发主机。打开 [http://127.0.0.1:8130/?demo=remapad-ui&width=240&height=280&density=1](http://127.0.0.1:8130/?demo=remapad-ui&width=240&height=280&density=1) 可以看到 240×280 画布、虚拟按键和运行日志。首次运行会用 Rust 的 `wasm32-unknown-unknown` target 构建 `pocketjs.wasm`，之后直接复用。

### 6. 编译 ESP-IDF 固件

从 ESP-IDF PowerShell 或已加载 `export.ps1` 的终端执行：

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
```

`firmware/CMakeLists.txt` 先按 `POCKETJS_ROOT`（默认仓库同级 `../pocketjs`）把 checkout 中的官方组件加入 `EXTRA_COMPONENT_DIRS`，`firmware/main/CMakeLists.txt` 再按顺序接入包：

1. 如果 `ui/dist/remapad-ui.pocket` 存在，使用官方 `pocketjs_embed_package`。
2. 否则使用官方 `pocketjs_compile_app`，让 CMake 调用 PocketJS CLI 生成 build 目录内的包。

建议先运行 `pnpm run build`，再运行 `idf.py build`。预构建路径只需要 Python 执行官方嵌入脚本，不需要 Bun；编译路径则需要可被 CMake 找到的官方 `pocket` CLI 和 Bun。构建产物是 `firmware/build/remapad_firmware.bin`，可直接用 `idf.py flash` 烧录。

### 7. 烧录与监视

```powershell
idf.py -p COM3 flash monitor
```

把 `COM3` 替换为实际端口。若开发板没有自动进入下载模式，按板卡说明操作 BOOT/EN。串口监视器使用 `Ctrl + ]` 退出。

## 关键文件

- [ui/pocket.json](../ui/pocket.json)：应用清单和应用侧 capability。
- [firmware/pocket.host.json](../firmware/pocket.host.json)：ESP32-S3 host profile。
- [firmware/CMakeLists.txt](../firmware/CMakeLists.txt)：按 `POCKETJS_ROOT` 发现官方 ESP-IDF 组件。
- [firmware/main/CMakeLists.txt](../firmware/main/CMakeLists.txt)：官方 package embed/compile 接入。
- [firmware/main/pocketjs_host.c](../firmware/main/pocketjs_host.c)：package、guest、binding、renderer、runner 生命周期。
- [firmware/sdkconfig.defaults](../firmware/sdkconfig.defaults)：N16R8 Flash/PSRAM 和 FreeRTOS 预设。
- [firmware/partitions.csv](../firmware/partitions.csv)：NVS、PHY 和 4 MB factory 分区。
- [scripts/pocketjs.mjs](../scripts/pocketjs.mjs)：官方 CLI、Web 开发主机和原生归档脚本的统一入口。
- [patches/README.md](../patches/README.md)：必须应用到 PocketJS checkout 的补丁与核对说明。
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

项目脚本通过 Bun 执行官方 checkout 中的 `tools/pocket.ts` 和 compiler。安装官方 Bun，并设置 `POCKETJS_ROOT` 指向包含该文件的 PocketJS checkout，确保 `bun` 位于当前 PowerShell 的 `PATH`，再重试 `pnpm run check` 或 `pnpm run build`。

### `pocketjs_compile_app requires the PocketJS CLI in PATH`

这是官方 CMake helper 的预期错误。优先在项目根目录执行 `pnpm run build` 生成 `ui/dist/remapad-ui.pocket`；如果要使用 CMake 自动编译路径，需要把官方 `pocket` CLI 放入 ESP-IDF 构建进程的 `PATH`，并确保它能定位 PocketJS framework checkout。

### 固件日志有 package admission 错误

确认 `.pocket` 是由同一份 `firmware/pocket.host.json` 生成的，且没有手动修改 profile 的视口、tick、presentation、raster density 或 capabilities。改动 profile 后重新执行 `pnpm run build`。

### 烧录后没有屏幕画面

当前固件只完成官方运行时和 RGB565 damage strip 的无面板 bring-up：`sample_input` 返回空输入，`after_turn` 尚未调用真实面板 DMA。需要根据开发板硬件资料补充产品 BSP，再将输入采样和 strip 传输接入回调。

### BLE 没有发现 NS2 手柄

当前固件尚未实现 USB→NS2→BLE 数据面，也没有配对广播或 GATT 服务。请先阅读 [controller.md](controller.md)，不要仅通过修改 PocketJS manifest 或 UI bridge 宣称已支持 NS2。

### `unsupported QuickJS source; review immutable-buffer patch before upgrading`

`pocketjs_guest` 的 QuickJS 源码校验值与 ESP Component Registry 提供的 `espressif/quickjs-ng` 0.14.0 不一致。按 [patches/README.md](../patches/README.md) 把 `patches/0001-quickjs-ng-0.14.0-source-pin.patch` 应用到 PocketJS checkout 后重新执行 `idf.py build`。

### `Missing libpocketjs_idf_ui_core.a for esp32s3`

官方组件没有附带 S3 原生归档，需要先执行 `pnpm run native` 用固定版本的 Xtensa Rust 生成。归档缺失时官方 CMake 会直接报错，不会尝试下载或安装工具链。

### Web 页面提示 `pocketjs.wasm not found`

官方 `hosts/web` 开发主机需要 Rust 构建的 wasm 核心。执行 `rustup target add wasm32-unknown-unknown` 后重试 `pnpm run dev`，脚本会在缺少 `pocketjs.wasm` 时调用官方 `tools/wasm.ts` 生成。

### `ui/dist` 或 `firmware/build` 出现文件

这些目录是生成目录，已被 Git 忽略。不要手动编辑其中的 JavaScript、PAK、`.pocket`、C/汇编嵌入源或生成头文件。

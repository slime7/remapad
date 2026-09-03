# Remapad ESP32-S3 N16R8

Remapad 是面向 ESP32-S3-WROOM-1 N16R8 的嵌入式控制器工程。最终产品接收 USB 输入，将其转换为 NS2 手柄报告，再通过 BLE 对外提供手柄服务；屏幕 UI 使用 PocketJS Vue Vapor，设备端使用 PocketJS 官方 ESP-IDF host 组件和 ESP-IDF 固件。

PSP 只作为 PocketJS 官方示例的架构参考，不是本项目的目标平台。ESP32-S3 的构建目标由 `firmware/pocket.host.json` 描述；不要使用 `pocket build --target psp` 生成本项目固件。USB→NS2→BLE 的协议、广播、GATT 和配对细节见 [controller.md](docs/controller.md)。

## 硬件规格

| 硬件项 | 参数 |
| :--- | :--- |
| 主控 | ESP32-S3-WROOM-1，Xtensa LX7 双核，最高 240 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB Octal PSRAM（OPI） |
| UI 视口 | 240 × 280，RGB565 |

## 架构

```mermaid
flowchart LR
    UI[ui/pocket.json + JSX] --> CLI[官方 PocketJS CLI]
    Profile[firmware/pocket.host.json] --> CLI
    CLI --> Artifacts[remapad-ui.js + remapad-ui.pak + remapad-ui.pocket]
    Artifacts --> CMake[ESP-IDF CMake]
    CMake --> Embed[官方 pocketjs_embed_package<br/>或 pocketjs_compile_app]
    Embed --> Host[pocketjs_host.c]
    Host --> Package[pocketjs_package]
    Package --> Guest[pocketjs_guest]
    Guest --> Binding[pocketjs_ui_qjs + ui_core]
    Binding --> Runner[pocketjs_runner]
    Runner --> Renderer[pocketjs_render_rgb565]
    Renderer --> Strip[RGB565 damage strip]
    Strip --> BSP[产品 BSP：面板 DMA]
    USB[USB 输入] --> DataPlane[产品数据面]
    DataPlane --> NS2[NS2 报告转换]
    NS2 --> BLE[BLE 广播 / GATT / 配对]
    DataPlane -.控制状态.-> Bridge[bridge 预留层]
```

职责边界如下：

- `ui/` 只描述应用、样式和资源，由官方 PocketJS 编译器生成包。
- `firmware/pocket.host.json` 是目标设备的事实源，描述视口、tick、presentation 和实际能力。
- `firmware/main/` 同时承载 PocketJS UI host 和未来产品数据面；UI runtime 负责渲染，USB/NS2/BLE 数据面负责高频报告转换，二者通过明确的设备状态边界协作。
- 当前仓库没有板卡引脚和 ST7789 控制器初始化信息，因此固件暂时完成无面板的 RGB565 frame bring-up；`sample_input` 也暂时返回空输入。

最终控制器数据面不应复用 PocketJS UI turn 作为高频报告通道。USB 接收、规范化、NS2 报告编码、BLE 广播/GATT 和配对状态机应在 ESP-IDF 原生任务与队列中实现；UI bridge 只承载设置、状态和诊断等低频控制消息。

## 目录结构

```text
remapad/
├── ui/
│   ├── pocket.json              # PocketJS 应用清单
│   ├── scripts/pocket.mjs       # 官方 CLI 的项目路径适配器
│   ├── scripts/dev.mjs          # WebAssembly 模拟器与热重载
│   └── src/                     # Vue Vapor JSX UI
│       └── bridge/              # USB/NS2/BLE 控制面协议预留
├── firmware/
│   ├── pocket.host.json         # ESP32-S3 host profile
│   ├── CMakeLists.txt           # ESP-IDF 工程入口
│   ├── sdkconfig.defaults       # N16R8 配置
│   ├── partitions.csv          # Flash 分区
│   └── main/
│       ├── idf_component.yml    # 官方 PocketJS 组件依赖
│       ├── CMakeLists.txt       # embed/compile 接入
│       ├── main.c               # 固件入口
│       ├── pocketjs_host.c       # 官方运行时生命周期与渲染回调
│       ├── bridge/               # 产品控制面预留
│       └── drivers/              # 背光、电池等 BSP 预留
└── docs/                        # 愿景、架构、抽象和上手文档
```

## 环境要求

- Node.js 18 或更高版本。
- pnpm，用于工作区依赖和脚本调度。
- Bun，用于执行 PocketJS 官方编译器。
- 更新的 PocketJS 官方源码 checkout；通过 `POCKETJS_ROOT` 指向其根目录，直到带 ESP-IDF host profile 的 CLI 发布到 npm。
- ESP-IDF `>=6.0,<6.2`，由官方 PocketJS ESP-IDF 组件要求。
- ESP32-S3 N16R8 开发板；实际屏幕和输入设备还需要产品 BSP。

Rust 运行时由 PocketJS 官方 ESP-IDF 组件的目标归档提供，使用注册表组件时不需要在本项目中维护 Rust 工程。

## 开发与构建

在仓库根目录执行：

```powershell
pnpm install
```

先在 PocketJS 官方 checkout 中安装其编译器依赖，再回到本项目；该安装只属于 PocketJS checkout：

```powershell
cd C:\src\pocketjs
bun install
$env:POCKETJS_ROOT = 'C:\src\pocketjs'
cd C:\src\remapad
```

然后在仓库根目录执行：

```powershell
pnpm run lint
pnpm run check
pnpm run compile
pnpm run build
pnpm run dev
```

`check`、`compile` 和 `build` 都通过 `ui/scripts/pocket.mjs` 调用该 checkout 中的官方 `tools/pocket.ts`，并自动传入 `firmware/pocket.host.json`。设置 `POCKETJS_ROOT` 后，`build` 的等价官方命令为：

```powershell
cd $env:POCKETJS_ROOT
bun tools/pocket.ts build --manifest C:\src\remapad\ui\pocket.json `
  --host-profile C:\src\remapad\firmware\pocket.host.json `
  --project-root C:\src\remapad\ui --outdir C:\src\remapad\ui\dist `
  --output C:\src\remapad\ui\dist\remapad-ui.pocket
cd C:\src\remapad
```

若已安装包含 host profile 支持的 `pocket` CLI，也可使用官方 CLI 形式：

```powershell
pocket build --manifest ui/pocket.json `
  --host-profile firmware/pocket.host.json `
  --project-root ui --outdir ui/dist `
  --output ui/dist/remapad-ui.pocket
```

输出位于 `ui/dist/`，包括 `remapad-ui.js`、`remapad-ui.pak` 和 `remapad-ui.pocket`。这些文件都是生成产物，不应手动编辑或提交。

载入 ESP-IDF 环境后构建固件：

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

当 `ui/dist/remapad-ui.pocket` 存在时，CMake 使用官方 `pocketjs_embed_package`，`idf.py build` 不需要 Bun。没有预构建包时，CMake 会按官方示例尝试使用 `pocketjs_compile_app`；此路径需要可从构建环境找到的官方 `pocket` CLI 和 Bun，因此团队构建建议先执行 `pnpm run build`。

## 分区与内存

当前 `firmware/partitions.csv` 采用官方示例同类的内置包方案：NVS、PHY 初始化和 4 MB `factory` 应用分区。`.pocket` 会嵌入 `factory`，不再需要独立的 SPIFFS 资源分区。

8 MB Octal PSRAM 用于 PocketJS guest 和渲染暂存区；真正的面板 DMA 缓冲区应由后续 BSP 按显示控制器和 ESP-IDF DMA 约束分配。

未来 BLE 配对凭证、主机绑定信息和控制器状态应使用 ESP-IDF NVS 等明确的持久化层管理，不能写入 PocketJS 包或依赖渲染任务的生命周期。协议字段和配对流程以 [controller.md](docs/controller.md) 为实现参考，并需通过真实设备抓包验证。

## 进一步阅读

- [产品愿景](docs/VISION.md)
- [系统架构](docs/ARCHITECTURE.md)
- [核心抽象](docs/ABSTRACTIONS.md)
- [上手指南](docs/GETTING-STARTED.md)
- [架构决策记录](docs/adr/README.md)
- [PocketJS ESP-IDF 官方指南](https://pocketjs.dev/docs/esp-idf/)
- [PocketJS 官方 ESP-IDF README](https://github.com/pocket-stack/pocketjs/blob/main/hosts/esp-idf/README.md)

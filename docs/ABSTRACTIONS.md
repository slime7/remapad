# Remapad 核心概念与领域抽象

本文档记录 Remapad 的两条数据路径：PocketJS 显示 UI 路径，以及 USB→NS2→BLE 控制器路径。PocketJS 包格式、C ABI、UI 输入编码和渲染指令不在项目内复制；需要调整时应以 PocketJS 官方 schema、组件头文件和 ESP-IDF 示例为准。NS2 协议、广播、GATT、HID 报告和配对内容见 [controller.md](controller.md)。

## 领域术语表

| 术语 | 含义 |
| :--- | :--- |
| **Pocket manifest** | `ui/pocket.json`，描述应用入口、框架、视口和 capability 要求。 |
| **Host profile** | `firmware/pocket.host.json`，描述设备实际提供的 ESP32-S3 host 能力和显示事实。 |
| **Pocket package** | `.pocket` 单文件包，包含 manifest 对应的构建计划、JavaScript、PAK 和目标 variant。由官方 CLI 生成，由 `pocketjs_package` 读取。 |
| **PAK** | PocketJS 资源包，承载样式、baked font atlas、图片等运行时资源。由 `pocketjs_ui_qjs_feed_pak` 提供给 binding。 |
| **Guest** | `pocketjs_guest` 创建的 QuickJS 执行环境，负责运行编译后的 JavaScript。 |
| **UI core** | `pocketjs_ui_core` 维护的 retained UI 节点、资源句柄、动画和 frame view。 |
| **UI binding** | `pocketjs_ui_qjs` 将 `globalThis.ui`、`globalThis.__pak` 和 UI turn 连接到 guest/core。 |
| **Damage region** | 一帧中需要重新光栅化的逻辑矩形；renderer 将其输出为 full-width RGB565 strip。 |
| **Host BSP** | 项目自己的 ESP-IDF 硬件层，负责面板、DMA、触控、按键、电源和其他外设。 |
| **USB input** | 由 ESP32 USB host 接收的外部输入报告，先进入产品数据面，不直接进入 PocketJS。 |
| **NS2 report encoder** | 将规范化控制器状态编码为目标 NS2 手柄的 USB/BLE 报告。 |
| **BLE controller peripheral** | 对 NS2 主机执行广播、GATT 服务、输入通知、输出命令和配对状态管理的 ESP32 外设角色。 |
| **Product control plane** | UI bridge 与固件控制面，用于低频状态、配置、配对操作和诊断；不承载高频输入报告。 |

## 应用清单与 host profile

应用和设备各自声明事实，官方 resolver 在构建时验证兼容性：

```text
ui/pocket.json                 firmware/pocket.host.json
  ├─ entry/framework             ├─ platform = esp-idf
  ├─ logical viewport            ├─ host ABI / tickHz
  ├─ requires                    ├─ physical/logical viewport
  └─ enhances                    ├─ presentation / density
                                 └─ capabilities
```

- `requires` 是应用运行所必需的能力，host 不提供时构建应失败。
- `enhances` 是应用可以利用但不应作为最低运行条件的能力。
- `capabilities` 只能填写固件确实会提供的能力。当前 Remapad profile 只声明 `text.glyphs.baked`；固件尚未接入真实触控、按键或模拟量采样，因此不能把这些能力写入 profile。
- profile 的 canonical hash 会进入构建计划和 package variant，运行时 `pocketjs_package_select` 会校验目标、ABI、tick、视口、density、presentation 和 profile hash。
- 当前设备的逻辑和物理视口均为 `240×280`。生成的 JavaScript bundle 可能仍包含官方 framework 的 `SCREEN_W = 480`、`SCREEN_H = 272` fallback 常量；它们不是设备 profile 的显示事实，也不应手动修改生成产物。ESP-IDF host 按 package contract 创建 `pocketjs_ui_core`，并通过 `globalThis.ui.__viewport` 发布 `240×280`；构建计划和运行时 frame 才是设备尺寸的校验依据。

当前应用仍可在浏览器 host 中使用触控模拟，因为浏览器 host 和设备 host 是两个不同的运行环境；ESP32 固件的空 `sample_input` 不会伪造触控能力。

## 最终产品控制器数据面

USB 到 NS2 BLE 的目标链路如下：

```text
USB HID / vendor report
          │
          ▼
USB 接收任务 → 报告解析 → 规范化 controller state
                                      │
                                      ▼
                              NS2 report encoder
                                      │
                                      ▼
              BLE 广播 / GATT / 输入通知 / 输出命令
                                      │
                                      ▼
                         NS2 主机的连接与配对
```

这条链路需要保持低延迟和确定性：

- USB 接收、解析、状态快照和 BLE 发送应使用 ESP-IDF 原生驱动、任务和队列。
- NS2 报告编码应按 [controller.md](controller.md) 的型号、Report ID、摇杆打包、震动输出和字节序实现，并用实机抓包验证。
- BLE manager 负责厂商广播字段、GATT service/characteristic、通知订阅、回连、唤醒和配对状态机；配对凭证通过 NVS 等持久化层保存。
- `ui/src/bridge/` 与 `firmware/main/bridge/` 只适合承载低频的模式切换、开始/停止配对、连接状态、电池和诊断消息。当前 bridge 保留但尚未编译或连接真实传输层。
- USB 高频输入不应经过 JSON bridge，也不应等待屏幕刷新或 JavaScript guest 执行。

## UI 图元与资源

`ui/src/App.tsx` 使用 PocketJS Vue Vapor 的 `<View>`、`<Text>` 和 `<Image>` 等图元：

- `<View>` 提供嵌入式布局、背景、边框、间距和 focusable 交互。
- `<Text>` 使用构建期收集的字符集和 baked font atlas；字号应使用 PocketJS 支持的 Tailwind 插槽。
- `<Image>` 通过资源名称引用 PAK 中的图像；图片在构建期处理，不在 ESP32 上解析 SVG。
- `createSpriteAnimation` 只描述资源帧选择，实际资源仍由官方编译器和 PAK 管理。

入口保持官方 Vue Vapor 形式：

```jsx
import { mount } from '@pocketjs/framework/vue-vapor';
import Hero from './App';

mount(() => <Hero />);
```

固件 `pocketjs_ui_qjs_mount` 会在应用 eval 前安装 `globalThis.ui` 和 `globalThis.__pak`；应用入口不再手动传入 PAK，也不依赖私有 prelude。

## 构建产物映射

```text
Vue Vapor JSX + pocket.json + host profile
                    │
                    ▼
          PocketJS 官方 compiler
                    │
       ┌────────────┼────────────┐
       ▼            ▼            ▼
remapad-ui.js  remapad-ui.pak  remapad-ui.pocket
                                   │
                                   ▼
                 pocketjs_embed_package / compile_app
                                   │
                                   ▼
                    firmware/build/pocketjs/remapad/
```

`firmware/build/pocketjs/remapad/` 中的 C/汇编嵌入文件和生成头文件都是 CMake 产物。项目不应再出现手写的 PCKT 解析、字节数组或 `app_pocket.h` 同步脚本。

## ESP-IDF 运行时生命周期

`firmware/main/pocketjs_host.c` 使用官方 C API，顺序与官方 ESP-IDF smoke 示例保持一致：

```text
embedded .pocket bytes
        │
        ├─ pocketjs_package_open
        └─ pocketjs_package_select(host contract)
                │ borrowed JS + PAK views
                ▼
        guest_create(QuickJS)
                │
        ui_core_create(contract viewport)
                │
        ui_qjs_create → feed_pak → mount → guest_eval
                │
        runner task:
          sample_input → pocketjs_ui_turn → after_turn
                                             │
                                  prepare damage plan
                                             │
                                  render_strip (RGB565)
                                             │
                                  panel transfer by BSP
                                             │
                                  commit / abort
```

包中的 JavaScript 和 PAK 都是借用视图，必须在 guest、binding 和 package 销毁前保持可读。生成的 package header/assembly 由 CMake 管理，因此不会发生 UI 与固件手动复制不一致的问题。

## 输入抽象

官方 `pocketjs_ui_input_t` 是一次 UI turn 的输入快照，包含：

- `buttons`：设备按键位图。
- `analog_x`、`analog_y`：左模拟量。
- `touches`、`touch_count`：当前触点数组。

输入采样属于 host/BSP，不属于 PocketJS 应用包。当前实现使用官方 runner 的 `sample_input` 回调并返回零按键、零模拟量、零触点；加入真实屏幕后，应把面板触控或设备按键转换为官方结构。USB→NS2 的高频状态应留在产品数据面，不应为了驱动 UI 而重新设计 PocketJS runtime 的输入协议。

## 渲染抽象

官方 RGB565 renderer 的职责是从 UI frame view 生成像素，不负责面板控制：

1. `pocketjs_rgb565_prepare` 生成 damage plan 并开始目标事务。
2. 对每个逻辑 damage region，分配或复用一个 full-width、region-height 的 RGB565 strip。
3. `pocketjs_rgb565_render_strip` 将 strip 写入调用方提供的缓冲区；容量必须精确匹配物理宽度乘以 region 高度。
4. BSP 将 strip 传给面板 DMA，所有传输成功后调用 `pocketjs_rgb565_commit`。
5. 任一渲染或传输失败时调用 `pocketjs_rgb565_abort`，不要提交不完整帧。

ESP32-S3 没有本项目使用的 P4 PPA 加速器，因此 renderer 使用官方软件 RGB565 路径。当前仓库只验证 strip 生成和事务，不宣称已经完成 ST7789 传输。

## 调度抽象

当前选择官方可选的 `pocketjs_runner`，由它创建固定 tick 的 FreeRTOS task，并按 host profile 的 `tickHz` 驱动 UI turn。它只负责调度和回调，不拥有输入驱动或显示设备。

若后续产品已有显示 task 或需要自定义调度，可移除 runner，直接在产品 task 中调用 `pocketjs_ui_turn`，但必须保留相同的 input snapshot、render transaction 和错误处理边界。

## 硬件扩展边界

产品 BSP 以后可以包含：

- ST7789 初始化、方向/偏移配置和 SPI/并口 DMA；
- 触控控制器、GPIO 按键和模拟量采样；
- USB host、输入报告解析和 NS2 报告编码；
- BLE 广播、GATT、配对/回连、震动命令和电源管理；
- 背光、电池和其他设备状态；
- 将上述事实映射到 `pocket.host.json` capabilities。

这些功能应直接使用 ESP-IDF 或对应官方驱动，并在对应的 BSP/data-plane 边界接入。没有真实硬件事实时，不在 UI manifest 或 host profile 中提前声明能力。协议字段和配对流程以 [controller.md](controller.md) 为参考，不应把文档中的实验性结论当作已完成的互操作保证。

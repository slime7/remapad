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
- `capabilities` 只能填写固件确实会提供的能力。当前 Remapad profile 声明 `text.glyphs.baked` 与 `input.touch`；后者随触摸 BSP（CST816T 采样，见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)）接入一并加入，按键和模拟量能力仍不在 profile 中。
- profile 的 canonical hash 会进入构建计划和 package variant，运行时 `pocketjs_package_select` 会校验目标、ABI、tick、视口、density、presentation 和 profile hash。
- 当前设备的逻辑和物理视口均为 `240×280`。生成的 JavaScript bundle 可能仍包含官方 framework 的 `SCREEN_W = 480`、`SCREEN_H = 272` fallback 常量；它们不是设备 profile 的显示事实，也不应手动修改生成产物。ESP-IDF host 按 package contract 创建 `pocketjs_ui_core`，并通过 `globalThis.ui.__viewport` 发布 `240×280`；构建计划和运行时 frame 才是设备尺寸的校验依据。

触摸预览页可以在浏览器中提供真实触点，浏览器 host 与设备 host 各自把输入交给同一套框架语义：预览页把指针事件转换为触摸帧，设备端由 `drivers/touch.c` 把 CST816T 采样填入 `sample_input`。

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
- BLE manager 负责厂商广播字段、GATT service/characteristic、通知订阅、回连、唤醒和配对状态机；配对凭证通过 NVS 等持久化层保存。凭证（每条 6B 主机 MAC + 16B LTK）是 111 字节小 blob、低频写（仅配对成功时一次），与 PHY 校准同住 NVS：NVS 的掉电一致性与磨损均衡正是为这类数据设计，不需要也不应迁往 storage 分区（其定位是大块通用数据）。凭证表最多 5 条、按 MAC 覆盖、满员淘汰最旧，配对新主机是追加而非覆盖，因此不常驻「解除配对」入口。
- flash 写入期间 cache 被禁用，而 PocketJS owner task 的栈在 PSRAM——从该任务直接执行任何 flash 写都会在禁缓存窗口访问 PSRAM 并触发 cache 异常重启（「停止配对即重启」的根因）。凭证等持久化写一律收敛到 `ble_creds` 的内部 RAM 栈写任务：各任务只更新内存表并投递快照，新增持久化需求必须沿用同一模式。
- `ui/src/bridge/` 与 `firmware/main/bridge/` 只适合承载低频的模式切换、开始/停止配对、连接状态、电池和诊断消息。传输层已接通：guest 侧 `HardwareDriver` 经 `globalThis.__nativeBridge.postMessage(json)` 发命令（由 `pocketjs_host.c` 在 mount 后、eval 前用 `pocketjs_guest_quickjs_install_once` 注入的 native surface 接收并入队），owner task 每帧 `js_bridge_service()` 处理队列并用 `pocketjs_guest_eval` 调 `__onNativeBridgeMessage(json)` 回发应答/事件；入队与出队都在 owner task 上，无锁。PWR 按键与串口 CLI 等非 owner task 上下文经 `js_bridge_submit_command` / `js_bridge_post_event` 的外部队列转移（guest eval 只允许在 owner task 上执行）。协议以 `ui/src/bridge/protocol.ts` 为准，命令包含 hello/getSystemStatus/setBacklight/setScreenPower/setUsbRole/getControllerConfig/setControllerConfig/startPairing/stopPairing/unpair/debugKey/reboot，事件包含 ready/systemStatus/backlightSet/screenPowerSet/screenPowerChanged/usbRoleSet/usbRoleChanged/controllerConfig/controllerConfigSet/pairingResult/unpairResult/pairingStateChanged/debugKeySet/rebooting/error。用户设置（背光亮度、USB 角色、手柄身份类型与配色）由 `firmware/main/config/app_config.c` 持久化到 NVS（内部 RAM 栈提交任务，与 ble_creds 同一模式），开机恢复。
- USB 角色（`usbRole`: device=插电脑 COM 口，host=插手柄）目前只由固件记录并如实上报 `usbRoleActive`；USB OTG PHY 切换属于数据面，未接入前任何代码都不触碰 RTC_CNTL USB mux，复位后永远回到默认的 USB-Serial/JTAG（COM 设备模式），"重启回 COM 模式"因此天然成立。
- 配对与连接状态已接入真实 BLE 会话（NimBLE 手柄外设，进度见 [ROADMAP.md](ROADMAP.md)）：bridge 的 `startPairing` 触发固件侧发现广播，`stopPairing` 退出配对模式、并断开已连接但注册握手未完成的主机（`pairing` 态由连接驱动，不断开则停止永远无法退出）；解除配对由显式 `unpair` 命令完成（清除 NVS 凭证并切回发现广播）。广播时机为开机即广播——有凭证发回连广播等待主机回连，无凭证发发现广播等待主机搜索，断开后按同一规则自动恢复。已连接但始终停留在握手等待态的主机（手机/PC 自动回连）由空闲超时主动断开（3 秒无协议活动，主机毫秒级初始化序列不受影响）；主机连接地址是随机地址，不能按 OUI 识别。配对成功的判定走协议证据——主机初始化/0x15 握手完成（或凭证匹配回连）记为主机已注册，NVS 凭证则是重启后仍成立的持久化证据，两者独立；配对六态由此实时推导并经 `pairingStateChanged` 推送，配对进行中 UI 底部导航锁定在配对页。Command 0x15 私有配对与 NVS 凭证见 [controller.md](controller.md) 与 [ADR 0010](adr/0010-nimble-ble-controller-stack.md)。电池字段仍是 `battery.c` 预留占位值（真实 ADC 随 M5 接入）。
- 调试注入是控制面进入数据面的唯一低频通道：bridge 的 `debugKey` 命令经 `dp_source_inject()` 在数据面当前输入状态上叠加一次按键按下并按时长自动释放（A/HOME 约 250ms，配对 L+R 约 1s，对应主机 Grip/顺序界面的配对确认动作；UI 调试页「按键指令」区），采样与编码仍由数据面任务独立完成，不引入高频路径。
- 输入获取与 NS2 输出已解耦为两个稳定接口（`firmware/main/dp/dp_source.h` 与 `firmware/main/ns2/ns2_output.h`，ADR 0011 边界内）：新增输入设备（USB 手柄、桥接 PC、UART 注入）只需实现 `dp_source_t` 并注册，首个注册源拥有摇杆/电池字段，后续源叠加按键，调试注入最后叠加；输出侧 `ns2_output_send()` 接收规范化状态（可只填需要输出的按键），内部按会话格式编码并经注册的输出通道（现役 BLE 通知，USB 预留）发送。主机下发的震动 / 玩家 LED / 触觉采样被 ble_session 解析为结构化事件（`ns2_rumble_event_t` 等）经反馈监听者分发，M5 起转发给插入的手柄或桥接 PC。电池经 `battery.c` 唯一入口 + `ns2_output_set_battery` 随报告上发；amiibo 镜像经 `ns2_output_amiibo_stage` 预置（传输方式待定），Report 0x09 的 NFC 状态字节随预置汇报。USB 输入/桥接的推进方案见 [usb-input-plan.md](usb-input-plan.md)。
- USB 高频输入不应经过 JSON bridge，也不应等待屏幕刷新或 JavaScript guest 执行。

## UI 图元与资源

`ui/src/App.tsx` 使用 PocketJS Vue Vapor 的 `<View>`、`<Text>` 和 `<Image>` 等图元：

- `<View>` 提供嵌入式布局、背景、边框、间距和 focusable 交互。
- `<Text>` 使用构建期收集的字符集和 baked font atlas；字号应使用 PocketJS 支持的 Tailwind 插槽。Inter 未映射的码点（中文等）经应用目录 `fonts.json` 声明的回退字体面（当前为 Noto Sans SC）烘焙进同一图集。
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

`firmware/main/pocketjs_host.c` 使用官方 C API，顺序与官方 ESP-IDF smoke 示例保持一致，但创建、mount、eval 和逐帧 turn 都在同一个产品 task 上完成：

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
        remapad-pjs owner task:
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

输入采样属于 host/BSP，不属于 PocketJS 应用包。当前实现由 owner task 的 `sample_input` 回调返回零按键、零模拟量、零触点；屏幕是触摸屏，接入后应把 CST816T 的采样转换为官方 `pocketjs_ui_touch_t` 触点数组，触点 `id` 在同一按压期间保持稳定、坐标使用逻辑像素（板卡引脚见 [hardware.md](hardware.md)）。USB→NS2 的高频状态应留在产品数据面，不应为了驱动 UI 而重新设计 PocketJS runtime 的输入协议。

## 渲染抽象

官方 RGB565 renderer 的职责是从 UI frame view 生成像素，不负责面板控制：

1. `pocketjs_rgb565_prepare` 生成 damage plan 并开始目标事务。
2. 对每个逻辑 damage region，分配或复用一个 full-width、region-height 的 RGB565 strip。
3. `pocketjs_rgb565_render_strip` 将 strip 写入调用方提供的缓冲区；容量必须精确匹配物理宽度乘以 region 高度。
4. BSP 将 strip 传给面板 DMA，所有传输成功后调用 `pocketjs_rgb565_commit`。
5. 任一渲染或传输失败时调用 `pocketjs_rgb565_abort`，不要提交不完整帧。

ESP32-S3 没有本项目使用的 P4 PPA 加速器，因此 renderer 使用官方软件 RGB565 路径。当前仓库只验证 strip 生成和事务，不宣称已经完成 ST7789 传输。

## 调度抽象

当前由产品自己的 `remapad-pjs` owner task 承担调度：它按 host profile 的 `tickHz` 驱动 UI turn，同时承载 guest 的创建、mount 和 eval。它只负责调度、输入采样回调与帧消费，不拥有输入驱动或显示设备。

不使用官方 `pocketjs_runner` 的原因是任务栈的宿主：`pocketjs_runner_config_t` 只能指定栈大小，FreeRTOS 任务栈始终由 IDF 从内部 RAM 分配，而 mount 需要的连续 C 栈空间超出内部 RAM 的可用容量。owner task 通过 `xTaskCreatePinnedToCoreWithCaps` 把栈放在 PSRAM。

**创建 guest 的任务和执行 UI turn 的任务必须是同一个。** QuickJS 的栈守卫以下限 `stack_top - stack_size` 判断溢出，而 `stack_top` 取自创建 runtime 时的栈指针，`JS_UpdateStackTop` 在官方组件中没有被调用。若 turn 换到别的任务执行，守卫量的是别人的栈，溢出不会被拦截。改动调度时这一点不能破坏；同时任务栈容量必须大于 guest 的 `stack_limit`。

## 硬件扩展边界

产品 BSP 以后可以包含：

- ST7789 初始化、方向/偏移配置和 SPI/并口 DMA；
- 触控控制器、GPIO 按键和模拟量采样；
- USB host、输入报告解析和 NS2 报告编码；
- BLE 广播、GATT、配对/回连、震动命令和电源管理；
- 背光、电池和其他设备状态；
- 将上述事实映射到 `pocket.host.json` capabilities。

这些功能应直接使用 ESP-IDF 或对应官方驱动，并在对应的 BSP/data-plane 边界接入。没有真实硬件事实时，不在 UI manifest 或 host profile 中提前声明能力。协议字段和配对流程以 [controller.md](controller.md) 为参考，不应把文档中的实验性结论当作已完成的互操作保证。

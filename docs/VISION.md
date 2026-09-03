# Remapad 产品愿景与边界

## 项目概述

Remapad 是一个面向 **ESP32-S3 (N16R8)** 的嵌入式控制器与 UI 系统。最终产品从 USB 接收输入，将其转换为 NS2 手柄报告，再通过 Bluetooth LE 对外提供手柄服务，同时在本机屏幕上显示连接、配对和设备状态。UI 使用 Vue Vapor + Tailwind，运行时使用 PocketJS 官方 ESP-IDF host 组件；控制器协议、广播、GATT 和配对范围记录在 [controller.md](controller.md)。

## 要解决的核心痛点

在传统的微控制器（MCU）UI 开发中，开发者普遍面临以下困境：
- **开发与调试链路冗长**：传统方案（如裸写 C/C++ 的 LVGL）每一次调整布局、微调文字或修改动效，都必须经过完整的固件交叉编译与物理串口烧录，开发效率极低。
- **缺乏声明式响应式状态绑定**：C 语言手写事件与状态同步代码繁琐且易产生内存泄漏与野指针崩溃，界面复杂后维护成本急剧上升。
- **WebView 方案资源过重**：ESP32-S3 仅有 512KB 片内 SRAM 与 8MB 片外 PSRAM，根本无法承载完整的 Chromium/WebKit 内核或包含 DOM 树的通用浏览器运行时。

Remapad 采用**“零 DOM、构建期光栅化、PC 仿真热重载”**的技术路径，从根本上解决上述痛点。

在控制器数据面，项目还需要解决 USB 输入设备格式不统一、NS2 报告编码复杂、BLE 广播/连接状态多，以及配对凭证持久化等问题。该数据面与 PocketJS UI runtime 解耦：高频输入和报告转发由 ESP-IDF 原生任务处理，UI 只观察状态并发送低频控制命令。

## 目标用户群体

- **物联网与智能硬件工程师**：需要为 ESP32-S3 智能硬件快速构建现代美观、高帧率 UI 的开发人员。
- **极客与创客群体**：希望制作便携桌面副屏、电子挂件、客制化小键盘或手持游戏/工具终端的开发者。
- **熟悉现代前端的技术人员**：希望使用习惯的 Vue/JSX 与 Tailwind 语法开发单片机应用，而无需深究底层硬件时序与寄存器的工程师。
- **需要 USB 转无线手柄网关的开发者**：希望将 USB 输入设备转换为 NS2 兼容的 BLE 手柄，并通过屏幕管理连接与配对状态。

## 核心目标与成功指标

1. **零刷机实时热重载**：在 PC 浏览器中通过 WebAssembly 提供与真机像素级一致的 60 FPS 实时仿真，保存代码后亚秒级刷新，UI 调试无需依赖硬件板卡。
2. **现代化的组件与样式体系**：支持 Vue Vapor 的 `ref` / `watchEffect` 响应式系统，全面支持 Tailwind 工具类，消除繁琐内联样式配置。
3. **固件构建链路清晰可复现**：前端通过官方 PocketJS CLI 和 ESP32-S3 host profile 生成 `.pocket`，ESP-IDF 通过官方组件嵌入该包；N16R8 内存配置和 240×280 视口由设备 profile 统一描述。
4. **USB 到 NS2 BLE 的可靠转发**：稳定接收 USB HID/原始报告，规范化输入状态，生成 NS2 手柄报告，完成 BLE 广播、连接、通知、配对和重连；协议细节以 [controller.md](controller.md) 为设计依据并以实机验证为准。
5. **极致轻量与高帧率**：在无硬件 2D 加速器（PPA）的 ESP32-S3 上，通过编译期静态光栅化实现高帧率流畅运行。

## 非目标与系统边界

为保持系统的轻量、专注与高确定性，Remapad 明确设立以下边界：
- **非通用 Web/移动端框架**：Remapad 专为小分辨率（如 240×280、320×240 等）嵌入式屏幕设计，不追求对 PC/手机通用复杂网页的大规模 DOM 与富文本排版支持。
- **不承载浏览器引擎**：运行时完全没有 HTML DOM 树、CSS 解析器或 JavaScript JIT 编译器，所有样式和字体均在编译阶段固化。
- **不替代嵌入式底层驱动**：Remapad 专注于 UI 视图与交互层，底层传感器采样、网络通信（Wi-Fi/BLE）与总线协议由 ESP-IDF 原生 C 模块负责。
- **不把控制器转发塞进 UI runtime**：USB 报告、NS2 编码、BLE GATT/广播和配对状态机属于产品数据面，不通过 PocketJS 的每帧 UI 接口承载。

## 核心设计原则

- **事实单一源 (Single Source of Truth)**：界面数据流始终由声明式响应式状态（Reactive State）驱动。
- **构建期计算优先 (Bake at Build-Time)**：凡能在 PC 构建期完成的工作（Tailwind 样式编译、TrueType 矢量字体图集烘焙、SVG 光栅化），绝不推迟到微控制器运行时消耗算力。
- **软硬件双向一致性 (Deterministic Parity)**：PC 端 WebAssembly 仿真器与 ESP32-S3 真机采用完全相同的渲染核心逻辑与字模度量规范，所见即所得。

## 当前阶段与演进规划

- **当前阶段 (Phase 1 - 官方 host 链路已接入)**：
  - 完成双工作区工程架构、N16R8 硬件预设和 ESP32-S3 host profile。
  - 使用 PocketJS 官方 ESP-IDF 组件完成 package、guest、UI binding、RGB565 renderer 和 runner 的生命周期接入。
  - 保留 60 FPS WebAssembly 浏览器模拟器与热重载，用于 UI 开发。
  - 固件目前完成无面板的首帧渲染 bring-up；ST7789 面板 DMA、USB 接收、NS2 报告转换、BLE 广播/配对和其他外设仍需产品 BSP/数据面实现。
- **近期演进规划 (Phase 2)**：
  - 丰富常用嵌入式基础组件库（列表滚动组件、开关 Switch、进度条 Progress、表单项）。
  - 按实际板卡补充 ESP-IDF 产品 BSP，实现面板提交、USB host 接收和 GPIO 等外设接入。
  - 按 [controller.md](controller.md) 完成 NS2 报告编码、BLE 广播/GATT、配对状态机和配对凭证持久化。

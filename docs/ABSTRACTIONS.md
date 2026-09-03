# Remapad 核心概念与领域抽象

本文档记录在跨前端、编译工具链及嵌入式固件时必须掌握的项目核心领域概念、数据契约与关键不变量。

---

## 领域术语表

| 术语 | 英文全称 | 概念解释 |
| :--- | :--- | :--- |
| **NodeMirror** | Node Mirror Tree | PocketJS 内部维护的虚拟镜像节点树结构，对应每个底层微控制器的物理渲染节点。 |
| **Pocket Package** | .pocket File | 符合官方规范的单文件应用归档格式（包含 PCKT 魔数、清单、变体表、代码段及资产段）。 |
| **Pak Container** | .pak File | 兼容 Dreamcart 规范的通用二进制键值资产包，存储样式表、点阵字模图集及光栅图片。 |
| **Font Atlas** | Baked Font Atlas | 矢量字体（如 Inter）在构建期针对特定字号和权重烘焙生成的单通道 8 位覆盖率点阵图集。 |
| **Style Record** | Binary Style Record | Tailwind 样式类经过解析与规范化后生成的定长二进制属性集合。 |
| **Touch Contact** | Touch Contact | 触控屏交互中携带屏幕坐标 (x, y)、触点 ID 与按压状态的数据包。 |
| **DeviceCmd** | Device Command | 前端通过桥接门面发送至硬件底层的控制指令，具备单调自增 id。 |
| **DeviceMsg** | Device Message | 硬件底层返回给前端的指令应答（携带请求 id）或主动广播事件（无 id）。 |

---

## 核心组件与图元抽象

在 [ui/src/App.jsx](../ui/src/App.jsx) 中，开发者通过调用 @pocketjs/framework/vue-vapor/components 提供的四种原子级嵌入式图元构建界面：

### 容器图元 (<View>)
- **功能**：整个布局系统的主力图元，基于 Flexbox 模型实现。
- **核心能力**：
  - 弹性排列（flex-row、flex-col）、对齐（items-center、justify-between）。
  - 内外边距、圆角（rounded-*）、阴影、背景渐变（bg-gradient-to-*）。
  - 承载触控点击事件（focusable 与 onPress 属性）。

### 文本图元 (<Text>)
- **功能**：文字内容展示节点。
- **核心特性**：
  - 不支持常规 Web 的任意内联 fontSize，字号必须映射至 Tailwind 支持的字模插槽（text-xs 对应 12px，text-sm 对应 14px，text-base 对应 16px，text-lg 对应 18px，text-xl 对应 20px）。
  - 内部文本在编译期自动提取字符编码（Codepoints）用于字体烘焙。

### 图像图元 (<Image>)
- **功能**：显示单张静态纹理或矢量光栅化图块。
- **引用机制**：通过 src="logo.png" 引用编译进 app.pak 的 ui:img.logo.png 纹理句柄。

### 精灵图元 (<Sprite>)
- **功能**：原生硬件帧动画图元。
- **驱动方式**：配合 createSpriteAnimation 使用，在底层驱动旋转与连续帧切换。

---

## 跨层映射与二进制契约

前端的高级声明式代码在构建过程中经历多重降维与二进制化，最终进入硬件：

```text
[Vue Vapor JSX] 
     │ (构建期提取类名与字符)
     ├──> [Tailwind 解析器] ──> styles.bin (styleId 索引映射) ──┐
     ├──> [Font 烘焙器]    ──> ui:font.<slot> (点阵字模图集)  ──┼──> app.pak
     └──> [SVG 光栅化器]   ──> ui:img.<name> (RGBA 纹理字节) ──┘      │
                                                                       ▼
[esbuild 打包] ──> app.js (IIFE 虚拟机字节流) ────────────────────> app.pocket (PCKT)
                                                                       │
                                                                       ▼
[C 语言数组生成] ──────────────────────────────────────────────> app_pocket.h
                                                                       │
                                                                       ▼
[ESP32-S3 Flash] <────── idf.py build / flash 静态嵌入 .rodata <───────┘
```

### .pocket 文件头格式 (PCKT)
前 16 字节固定包头：
- 0x00..0x03：魔数 0x544b4350（ASCII 字符 PCKT）。
- 0x04..0x07：版本号（当前固定为 1）。
- 0x08..0x0B：清单 JSON 字节长度。
- 0x0C..0x0F：变体目标数（如针对 esp32s3 为 1）。
- 包末尾 8 字节：FNV-1a 64 位整包完整性校验码。

### 固件常量头契约 ([firmware/main/app_pocket.h](../firmware/main/app_pocket.h))
前端构建器负责将整个 app.pocket 二进制数据无损输出为标准 C 常量：
```c
#pragma once
#include <stdint.h>
#include <stddef.h>

const size_t app_pocket_len = 554384;
const uint8_t app_pocket_data[554384] = {
  0x50, 0x43, 0x4b, 0x54, ...
};
```

---

## 生命周期与事件交互模型

### 应用挂载周期 (mount)
在 [ui/src/index.jsx](../ui/src/index.jsx) 中：
```javascript
import '@pocketjs/framework/prelude';
import { mount } from '@pocketjs/framework/vue-vapor';
import Hero from './App.jsx';

mount(() => <Hero />, {
  pak: typeof globalThis !== 'undefined' ? globalThis.__pak : undefined,
});
```
- prelude：初始化轻量虚拟 DOM 门面（globalThis.__pocketDocument），拦截原生 DOM 创建。
- mount：首先解析 pak 数据，通过 loadStyles 注册样式表，通过 loadFontAtlas 注册字模，通过 uploadTexture 上传图像，最后启动渲染循环。

### 帧驱动与触控处理流水线
渲染核心保持 60 Hz 恒定主循环：
```javascript
globalThis.frame(buttons, analog, touches, hits, touchSurfaces);
```
1. **触控点捕获**：触控屏将屏幕像素物理点转换为 9 位打包坐标 packed = (x & 511) | ((y & 511) << 9) | ((id & 255) << 18)。
2. **两阶段点击**：
   - 触碰按下帧：下发 [packed] 接触点，底层命中测试（hitTest）锁定焦点。
   - 触碰抬起帧：下发 [] 释放信号，手势状态机判定触发 onTap -> 调用对应节点的 onPress 回调。
3. **响应式驱动更新**：状态更新后，Vue Vapor 精确触发局部重绘，驱动动效与文本即时重写。

---

## 硬件桥接契约与通信抽象 (Hardware Bridge)

借鉴 PocketJS 官方 PSP 示例项目（`pocket-youtube`）的跨进程/跨层通信模型，Remapad 在 [ui/src/bridge/](../ui/src/bridge/) 与 [firmware/main/bridge/](../firmware/main/bridge/) 间建立了双向解耦机制：

### 协议定义层 ([ui/src/bridge/protocol.ts](../ui/src/bridge/protocol.ts))
- **请求负载 (DeviceCmd)**：携带自增序号 `id`，涵盖背光设定、震动触发、电量查询及 Switch 2 配对控制。
- **应答负载 (DeviceMsg)**：
  - 针对请求的响应：回显相同 `id`，实现无序异步解析。
  - 硬件主动推送：无 `id`，如物理按键中断事件（`buttonEvent`）或低电量告警（`lowBatteryAlert`）。

### 统一驱动门面 ([ui/src/bridge/driver.ts](../ui/src/bridge/driver.ts))
- 内部自动检测运行环境：真机环境下将消息序列化投递给固件注入的原生通道（`__nativeBridge.postMessage`）；浏览器仿真环境下自动转交本地虚拟外设分发器（`mock.ts`）。
- 前端组件统一调用 `hardware.send()` 与 `hardware.onEvent()`，与具体运行环境无感。

### Switch 2 手柄协议技术规范
- 详见 [docs/controller.md](controller.md)，涵盖 Switch 2 手柄 BLE 5.x 私有 GATT 属性表、64 字节配对凭证存储布局、0x30/0x31 高速 HID 输入报告格式及 HD 震动指令集。

# 0003 — 采用官方 PocketJS ESP-IDF host 构建链路

- 状态: active
- 日期: 2026-09-03
- 替代: 0002

## 背景

PocketJS 已提供官方 ESP-IDF 组件、host profile、包准入校验和可选 runner。原工程手写 PCKT 打包器和 C 头文件同步链路与官方 ESP-IDF 集成不一致，也无法表达真实的屏幕和输入能力。Remapad 的最终产品还需要 USB→NS2→BLE 数据面及 BLE 配对/广播；现有 bridge/drivers 作为产品控制面预留保留，但不应冒充 PocketJS UI runtime 或已完成的硬件实现。

## 决策

以 `firmware/pocket.host.json` 作为设备事实源，使用官方 `pocketjs_package`、`pocketjs_guest`、`pocketjs_ui_core`、`pocketjs_ui_qjs`、`pocketjs_render_rgb565` 和 `pocketjs_runner`；由 `firmware/main/CMakeLists.txt` 通过 `pocketjs_embed_package` 或 `pocketjs_compile_app` 接入官方构建链路。该决策仅取代 0002 中的自定义打包与 host 接入范围，0002 关于硬件 bridge 和控制面分层的部分继续有效。产品固件另外负责 USB 输入、NS2 报告转换、BLE 广播/GATT/配对、状态持久化、输入采样和显示提交；控制器协议见 [controller.md](../controller.md)。

## 考虑的方案

- 官方 host profile 加官方 ESP-IDF 组件（采用）
- 继续维护自定义 PCKT 格式和 C 数组（放弃）；产品 bridge 仍作为独立控制面预留
- 使用 `pocket build --target psp` 生成固件（不适用，PSP 是另一平台后端）

## 影响

- 包格式、ABI、视口、tick 和 capability 校验交给官方实现，减少重复代码并保持与 PocketJS 更新同步。
- idf.py build 在已有 .pocket 时不需要 Bun；缺少预构建包时，官方 CMake helper 需要 PATH 中可用的 pocket CLI 和 Bun。
- 当前 firmware 只完成无面板提交的 RGB565 frame bring-up；实际 ST7789 初始化、DMA 传输、USB 输入、NS2 报告、BLE 广播/GATT/配对和触控采样必须由后续产品 BSP/数据面按硬件资料和 [controller.md](../controller.md) 实现。

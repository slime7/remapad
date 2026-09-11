# 0004 — PocketJS 组件与原生归档改由本地 checkout 提供，Web 预览切换为官方开发主机

- 状态: active
- 日期: 2026-09-11
- 替代: 无

## 背景

此前固件只能停留在无法构建的状态：firmware/main/idf_component.yml 声明的 pocket-stack/pocketjs_* 组件并未发布到 ESP Component Registry，firmware/components/ 下的组件副本又与官方实现逐步漂移，其中 pocketjs_guest 的 QuickJS 源码哈希已经过期。ESP Component Registry 当前提供的 espressif/quickjs-ng 0.14.0 源码哈希与 pocketjs_guest 使用的校验值也不一致，configure 阶段会直接失败。前端方面，仓库自建的 WASM 模拟器页面和 ui/scripts 适配层与官方 hosts/web 开发主机并存，两套预览路径都要自行维护。

## 决策

把 PocketJS 组件、编译器和开发主机统一指向本地官方 checkout：POCKETJS_ROOT 决定 checkout 位置，firmware/CMakeLists.txt 用 EXTRA_COMPONENT_DIRS 发现其中的 hosts/esp-idf/components，删除 firmware/components 副本；ESP32-S3 的原生 Rust 归档用官方 tools/esp-idf-native.ts 配合固定 Xtensa Rust 生成；Web 预览改用官方 tools/pocket.ts compile 加 hosts/web/serve.ts，由 scripts/pocketjs.mjs 提供统一入口；QuickJS 源码哈希差异以 patches/ 中的补丁应用到 checkout，不修改组件语义。

## 考虑的方案

- ESP Component Registry 的官方组件（当前未发布，且 registry 版 quickjs-ng 与组件校验值不一致）
- 继续在 firmware/components/ 维护组件副本（与官方实现漂移，且这次已导致固件无法配置）
- 本地官方 checkout 加 EXTRA_COMPONENT_DIRS 与官方构建脚本（采用）

## 影响

- 固件与 UI 都使用官方实现，组件和编译器升级只发生在 PocketJS checkout；2.16 MB 与 2.24 MB 的 S3 原生归档需要一次性用固定 Rust 工具链构建。代价是构建环境必须存在可用的 PocketJS checkout，ESP-IDF 构建不再是自包含仓库；QuickJS 补丁在 PocketJS 更新该常量或更换 quickjs-ng 版本后必须移除。

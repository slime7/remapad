# 0005 — ESP-IDF 组件与原生归档固定在本仓库，pocketjs 仅作开发参考

- 状态: active
- 日期: 2026-09-11
- 替代: 0004（仅取代固件组件与原生归档的来源；0004 关于 Web 预览采用官方 hosts/web 的范围继续有效）

## 背景

上一版决策把 ESP-IDF 组件和原生归档放在 PocketJS checkout，结果两项本项目的成果落在了别的仓库里：2.16 MB 与 2.24 MB 的 S3 原生归档、以及 QuickJS 源码校验的修正都写在 checkout 中，remapad 自己无法独立构建。同时 npm 上发布的 @pocketjs/framework 0.11.0 不含 ESP-IDF host profile 编译器，前端编译只能来自本地 checkout，而该包自带的 hosts/web 与 pocketjs.wasm 已经能承担 Web 预览。

## 决策

固件侧自包含：六个官方 ESP-IDF 组件连同 ESP32-S3 原生归档固定在本仓库 firmware/components/，ESP-IDF 依靠默认组件目录直接发现它们，构建不需要 Rust 也不需要 PocketJS checkout；QuickJS 源码校验值在仓库内副本中直接修正。前端侧使用项目依赖 ui/node_modules/@pocketjs/framework 提供编译器与 hosts/web 开发主机，Web 主机的 dist/ 通过 junction 指向本仓库 ui/dist，使包与预览产物都留在项目内。POCKETJS_ROOT 降级为可选：只在当前 npm 版本缺少 host profile 编译器、或需要重建原生归档与登记上游更新时指向官方 checkout，该目录不再承载本项目的任何产物。

## 考虑的方案

- 组件与归档继续留在 PocketJS checkout（本项目的成果落在参考仓库，remapad 无法独立构建）
- 把组件与归档固定进 remapad，checkout 仅作参考（采用）
- 等官方把组件发布到 ESP Component Registry 后改用注册表依赖（当前未发布，registry 版 quickjs-ng 与组件校验值也不一致）

## 影响

- 克隆 remapad 并安装依赖后即可完成 UI 编译、Web 预览和固件构建，构建不再依赖机器上另一份 PocketJS 源码。代价是六个组件与两个原生归档（约 4.5 MB）随仓库维护，升级时要主动同步上游；上游或 Registry 的 quickjs-ng 内容变化后，需按 patches/README.md 重新对账并刷新 build-receipt.json。

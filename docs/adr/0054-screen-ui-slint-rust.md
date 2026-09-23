# 0054 — 屏幕 UI 改用 Slint + Rust，固件不再挂 JS 运行时

- 状态: active
- 日期: 2026-09-23
- 替代: 0001

## 背景

屏幕 UI 原先跑 PocketJS：界面在 JS（Vue Vapor）里写，经 QuickJS guest 解释执行，宿主与渲染回调留在 C 侧。
界面、平台与宿主分属两套语言，改一处界面要跨 JS / C 两层调试，组件与脚本还依赖 Registry 的 C++ 预编译库。
屏幕这条通路的约束是已知的：S3 上只有软件渲染器可用，字形必须在构建期烘成位图（运行期不解析字体文件），
4 MB 应用分区要同时放界面与固件。

## 决策

屏幕 UI 改用 Slint：ui/ 里是 .slint 源码，firmware/components/slint_ui 用 slint-build 在构建期把它编译成 Rust，
再交叉编译成静态库链进固件，运行期用 Slint 的软件渲染器画到面板；界面状态由固件经 C ABI 写入，动作经回调交回。
Rust 侧零 unsafe：业务逻辑全部 safe（crate 级 deny），unsafe 只留在 C ABI 边界并逐处注明原因，
alloc 的内存出口（__rust_alloc 等四个符号）由 src/rust_heap.c 提供。
字形在构建期按界面用到的字符自动子集烘成位图（图标与转圈都走字形），
运行期才拼出来的字符串靠 app.slint 的锚点串钉住码点。
界面用例是宿主侧 #[test]（Slint 测试后端 + 软件渲染器），不再保留真机用例台。

## 考虑的方案

- Slint + Rust：界面、平台层与宿主层收敛成一种语言，构建期编译与烘字形，依赖 crates.io 的 slint 与 Espressif 的 xtensa Rust 工具链（采纳）
- 继续 PocketJS + JS：界面留在 JS、宿主留在 C，改一处界面要跨两层调试，且依赖 Registry 的预编译库（否决）
- LVGL 或自绘 + C：控件、布局与中文排版都要重做，焦点环与拖动这些已有实现没有对应物（否决）
- 沿用上一代的 C++ 预编译 UI 库：工具链多一套 C++ 预编译产物，构建不再只靠 cargo 与 pnpm（否决）

## 影响

- 正面：屏幕通路只剩 Rust 与一层 C ABI；界面用例在开发机上跑真实 .slint 产物（cargo test --manifest-path ui/Cargo.toml），改界面不必等烧录。
- 正面：预览改由 slint-viewer 打开 ui/preview.slint（scripts/ui-preview.py）：设备画面 240 × 280 在上、控制条在下，
  动作在预览里按固件语义结算，不再需要浏览器预览页与 wasm 核心。
- 成本：固件多带一份 Rust 静态库（应用镜像约 2.2 MB，4 MB 分区余量 44%）；构建多一层 xtensa Rust 工具链（换机步骤见 ui/README.md 与 scripts/setup-rust-toolchain.py）。
- 约束：Slint 平台层按单线程前提实现，界面状态只在 UI 任务上访问，跨任务只经状态轮询与原子标记。
- 约束：没写进 .slint 字面量的字符不会被烘成字形，固件回发的文本要么落在锚点串里，要么上屏是空洞。
- 约束：PocketJS 运行时的组件、脚本与包链路（firmware/components/pocketjs_*、patches/、scripts/pocketjs.mjs 等）已整体移除，仓库不再有 Node.js / Bun。
- 门禁：显示通路的条带划分与刷新取值以 docs/ARCHITECTURE.md 为准；ADR 0017、0049、0051、0052 描述的是 PocketJS 渲染器，已随本次切换作废。

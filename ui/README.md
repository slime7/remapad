# ui：屏幕界面（Slint）

屏幕 UI 工作区包含界面源码（`src/`）、静态资源（`assets/`）、宿主测试（`host/`）、固件组件（`slint_ui/`）与 WASM 预览（`preview/`）。

## 目录

| 路径 | 内容 |
| :--- | :--- |
| `src/app.slint` | 界面主体 `AppContent`（页表、四叶草轮播带、拖动切页、确认弹窗与全屏遮罩）与设备侧入口窗口 `App` |
| `src/pages.slint` | 八个页面（亮度、手柄、配对、电源、USB 模式、DS 设置、系统信息、无线调试）与调试页 |
| `src/components.slint` | 复用控件：底栏、状态图标、确认弹窗、拖动层 |
| `src/theme.slint` | 配色与字号常量 |
| `host/` | 宿主用例包：编译界面并给用例提供元素几何与画面量测工具 |
| `host/tests/*.rs` | 宿主用例：注入界面状态、按元素几何与像素断言屏幕行为（含预览窗自己的用例），见 [docs/TESTING.md](../docs/TESTING.md) |
| `preview/` | 浏览器预览包：设备画面编成 wasm32 供浏览器预览与端到端用例；`wasm.slint` 是窗口壳，`preview-core.slint` 是预览核心（模拟状态 + 动作结算），`index.html` 是预览页（物理屏外观 + HTML 控制台），`tools/` 是构建与服务脚本，`e2e/` 是 Playwright 用例，`dist/` 是构建产物（已忽略） |
| `slint_ui/` | 固件 Rust 界面组件（ESP-IDF 组件，目录名即组件名）：平台层、宿主层、C ABI、装配层 `ui_host.c`、启动画面 `boot_splash.c` 与 `rust_heap.c` |
| `render-plan/` | 行带计划：damage 裁剪、行带切分与逐行拷贝，平台层与宿主用例跑同一份实现 |
| `build-support/` | 界面编译口径单一来源：风格、字号表与字体路径，`host` 与 `slint_ui` 的 `build.rs` 共用 |
| `assets/fonts/` | 正文字体（NotoSansSC）、图标字体（MaterialIcons）与转圈字体（seguisym） |
| `assets/main.svg`、`assets/dock.svg` | 四叶草轮播带底图（一个卡片步距里的周期图，平铺出两侧露边的静止画面）与底栏底图 |

界面文案必须写在 `.slint` 里：字形在构建期按字号烘成位图，固件经 bridge 回发的文本不会被烘焙，直接上屏是方框。

## 构建链路

1. 将 `src/app.slint` 编译为 Rust 代码并光栅化嵌入资源。
2. 提取界面用到的字符子集，按 12/14/16/24 字号烘焙字形位图。
3. 交叉编译为 `xtensa-esp32s3-none-elf` 静态库并由 ESP-IDF 链接。

## 宿主用例

使用宿主环境运行界面测试，按元素几何与画面像素断言：

```powershell
cargo test --locked --manifest-path ui/Cargo.toml              # 全量
cargo test --manifest-path ui/Cargo.toml --test bottom_bar 底栏  # 只跑匹配的用例
```

## WASM 预览与浏览器端到端用例

```powershell
cd ui/preview
node tools/build.mjs                    # 构建预览产物到 dist/（首次自动补 wasm32 target 与 wasm-bindgen-cli，要等几分钟）
pnpm dev                                # 起服务：http://127.0.0.1:8123 浏览器打开即预览；存盘自动重编并整页刷新
pnpm test                               # 起服务 + 跑 Playwright 用例
```

`pnpm dev` 监听界面源码（`.slint`、wasm 包源码、编译口径与字体底图）与 `index.html`，变更后自动重编 dist；
构建失败在页面右下角浮层显示日志尾，改回来源码即恢复。端到端用例的 webServer 走同一服务、不带监听。

## 前置环境：Xtensa Rust 工具链

构建界面需要 Xtensa Rust 工具链（`esp`）与 `rust-src` 组件。

```powershell
uv run python scripts/setup-rust-toolchain.py          # 自动安装或补全缺失组件
uv run python scripts/setup-rust-toolchain.py --check  # 仅检查
```

手动安装步骤：
```powershell
cargo install espup --locked
espup install -t esp32s3
```
## CMake 变量

| 变量 | 默认值 | 作用 |
| :--- | :--- | :--- |
| `REMAPAD_UI` | `ON` | 是否编入屏幕 UI；OFF 时纯 C 构建固件（不需要 Rust 工具链，屏幕熄灭、设置走串口 CLI） |
| `REMAPAD_SLINT_RUST_TOOLCHAIN` | `esp` | 构建时调用 `cargo +<名字>`；工具链换了名字，或本机留了多套 esp 工具链时改它 |
| `REMAPAD_SLINT_FONT` | `ui/assets/fonts/NotoSansSC-Regular.otf` | 构建期烘字形用的字体文件 |

`REMAPAD_SLINT_RUST_TOOLCHAIN` 有两种给法，都在 CMake 配置阶段读取，环境变量优先：

```powershell
cd firmware
$env:REMAPAD_SLINT_RUST_TOOLCHAIN = "my-esp"
idf.py reconfigure
idf.py -DREMAPAD_SLINT_RUST_TOOLCHAIN=my-esp build
```

## 常用命令

```powershell
idf.py build                # 完整编译
idf.py -p COMx app-flash    # 仅烧录应用分区
```

## 改动注意

- 文案定义在 `.slint` 中，字号使用 `theme.slint` 预设档位（12/14/16/24）。
- 单色图标码点对照表：

  | 码点 | 字形 | 用在哪 |
  | :--- | :--- | :--- |
  | U+E30C / U+E99D | desktop_windows / desktop_access_disabled | 底栏 USB 模式格（PC 已连 / 未连） |
  | U+E338 / U+E500 | videogame_asset / videogame_asset_off | 底栏 USB 模式格（手柄已插 / 未插）、模式页手柄卡片 |
  | U+E701 | missing_controller | 底栏主机连接格 |
  | U+E1D8 U+EBE1 U+EBD6 U+EBE4 / U+E1DA | signal_wifi_4_bar、network_wifi_3/2/1_bar / signal_wifi_off | 无线调试页左上角信号图标：已连接按 RSSI 分四档（-55 / -66 / -77 为界），连接中、断开与读不到 RSSI 画关闭 |
  | U+F30D U+F30C U+F30B U+F30A U+F309 U+F308 U+F307 U+F304 | battery_android_0 … _6 与 full | 底栏电量格：每 15% 一档，100% 给满格（15% 以下整体转错误色） |
  | U+EECB U+EECA U+EEDE U+EEDB | gamepad 方向与肩键 | 底栏手柄提示行的「翻页」 |
  | U+EEC9 U+EECC | gamepad 上下 | 底栏手柄提示行的「选择」 |
  | U+EECE / U+EED0 | gamepad_circle_right / _down | 底栏手柄提示行的「确认 / 退出」 |
  | U+2801 / U+2802 / U+2804 / U+2840 / U+2880 / U+2820 / U+2810 / U+2808 | 盲文点阵八帧 | 配对页状态行的转圈 |
- 界面状态与动作仅在 UI 任务中访问，跨任务交互通过状态轮询与原子标记交接。

# ui：屏幕界面（Slint）

本目录是屏幕 UI 的 Slint 工作区：界面源码在 `src/` 与 `assets/`，固件把 `slint_ui/` 里的 Rust 组件编进镜像，
同一份源码也由 `host/` 的宿主用例包编译验证；四份 Rust 包共用根 `Cargo.toml` 的一份 `Cargo.lock`。

Rust 组件、工具链与日常命令见下面各节。

## 目录

| 路径 | 内容 |
| :--- | :--- |
| `src/app.slint` | 界面主体 `AppContent`（页表、四叶草轮播、拖动切页、确认弹窗与全屏遮罩）与设备侧入口窗口 `App` |
| `preview.slint` | PC 交互预览窗：设备画面 + 控制条，动作在预览里结算；只给预览用，不参与固件构建 |
| `src/pages.slint` | 七个页面（亮度、手柄、配对、电源、USB 模式、DS 设置、系统信息）与调试页 |
| `src/components.slint` | 复用控件：底栏、状态图标、确认弹窗、拖动层 |
| `src/theme.slint` | 配色与字号常量 |
| `host/` | 宿主用例包：编译界面并给用例提供元素几何与画面量测工具 |
| `host/tests/*.rs` | 宿主用例：注入界面状态、按元素几何与像素断言屏幕行为（含预览窗自己的用例），见 [docs/TESTING.md](../docs/TESTING.md) |
| `slint_ui/` | 固件 Rust 界面组件（ESP-IDF 组件，目录名即组件名）：平台层、宿主层、C ABI、装配层 `ui_host.c`、启动画面 `boot_splash.c` 与 `rust_heap.c` |
| `render-plan/` | 行带计划：damage 裁剪、行带切分与逐行拷贝，平台层与宿主用例跑同一份实现 |
| `build-support/` | 界面编译口径单一来源：风格、字号表与字体路径，`host` 与 `slint_ui` 的 `build.rs` 共用 |
| `assets/fonts/` | 正文字体（NotoSansSC）、图标字体（MaterialIcons）与转圈字体（seguisym） |
| `assets/main.svg`、`assets/dock.svg` | 四叶草卡片底图与底栏底图 |

界面文案必须写在 `.slint` 里：字形在构建期按字号烘成位图，固件经 bridge 回发的文本不会被烘焙，直接上屏是方框。

## 构建链路

`ui/slint_ui` 是 Rust 组件（固件 CMake 经 `EXTRA_COMPONENT_DIRS` 引入），`idf.py build` 为它做三件事：

1. 用 `slint-build` 把 `src/app.slint` 编译成 Rust 代码（含 SVG 光栅化与资源嵌入）。
2. 按 `build.rs` 的字号表（12/14/16/24）烘字形位图：正文用 `ui/assets/fonts/NotoSansSC-Regular.otf`，
   图标用 `ui/assets/fonts/MaterialIcons-Regular.ttf`，转圈用 `ui/assets/fonts/seguisym.ttf`；
   烘制按「界面里出现过的字符」自动子集，字体文件本身不进固件，仓库里也不留派生产物。
3. 交叉编译成 `xtensa-esp32s3-none-elf` 静态库链进固件；运行期用 Slint 的软件渲染器画到面板，不解析字体文件。

新增或改名 `.slint` 与 `assets/*.svg` 由 CMake 的 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 跟踪，改完直接 `idf.py build`。

首次构建要从 crates.io 拉取依赖（版本由 `Cargo.lock` 锁定，`cargo build --locked` 不会改动它），之后走本地缓存。

## 宿主用例

`host/` 是宿主侧用例包：`build.rs` 经 `build-support` 用与固件组件同一套口径编两份源码
（`src/app.slint` 给设备侧用例、`preview.slint` 给预览用例；同字体、同字号表，额外打开元素调试信息），
`host/tests/*.rs` 装 Slint 测试后端注入界面状态、用软件渲染器渲染一帧，按元素几何与画面像素断言。
workspace 的 `default-members` 只有 host，日常命令不会去碰 no_std 的固件包。

```powershell
cargo test --locked --manifest-path ui/Cargo.toml              # 全量
cargo test --manifest-path ui/Cargo.toml --test bottom_bar 底栏  # 只跑匹配的用例
```

只用到宿主 stable 工具链，xtensa 工具链只服务于上面的固件构建。界面里的 id 是用例的查询入口（例如 `BottomBar::battery-text`），改 id 要同步改 `tests/`。
断言口径、覆盖范围与「哪些元素查得到」的注意事项见 [docs/TESTING.md](../docs/TESTING.md)。

## PC 交互预览（slint-viewer）

开发机上用官方 slint-viewer 打开 `preview.slint`：上半是 240 × 280 的设备画面（与固件同一棵 `AppContent`），下半是控制条；
改完任意 `.slint` 存盘即刷新（走 `--auto-reload`）。

```powershell
cargo install slint-viewer --version 1.18.1 --locked             # 只需装一次；版本与界面用的 Slint 对齐
uv run python scripts/ui-preview.py                               # 交互预览（设备画面 + 控制条）
uv run python scripts/ui-preview.py --file src/app.slint          # 只看设备画面（240 × 280）
uv run python scripts/ui-preview.py --check                       # 只编译并打印诊断
uv run python scripts/ui-preview.py --screenshot agent-temp/ui.png # 渲染一帧存图后退出
```

设备画面里的控件点下去照常发动作，动作由预览窗按固件语义结算（与固件核心 `firmware/main/ui/ui_service.c` 的动作分发同名同参），
所以能点着走一遍界面：翻页、拖动切页、亮度加减、确认弹窗、配对档位、USB 角色与 OTA 进度都能在 PC 上看；
焦点环用控制条的「手柄操控 / 焦点 ± / 确认键」走查（确认键转发给设备画面自己的按页分发）。
「焦点 ±」在可聚焦项之间循环（到底再按回到另一端，与实机的上下键一致）。
控制条还给出这些状态：命令提示（1-4）、手柄家族（PAD / PS / XBOX / NS / STEAM）、PC 连接、玩家灯掩码与调试页注入高亮；
最后两行是当前读数与最近一次动作，动作看着没生效先看它。

脚本把 `SLINT_DEFAULT_FONT`（`assets/fonts/NotoSansSC-Regular.otf`）与字号表（12 / 14 / 16 / 24）
按固件构建的同名变量喂给编译器，并按 `SLINT_SCALE_FACTOR=1` 打开窗口：中文、图标与排版与实机同源，
设备画面就是 240 × 280 个物理像素（控制条画在它下面）。想放大看细节就自己设 `SLINT_SCALE_FACTOR`。

预览里的电池、内存、版本号与蓝牙地址都是模拟值：预览验的是界面与动作结算，不验固件行为；
固件行为要么在宿主用例里断言，要么在实机上验（串口 `key ui` / `ui on|off` 进出手柄操控窗口）。
新增控件时把动作名写成固件 `ui_action` 里的那个，并在 `preview.slint` 的动作结算里补一条分支。

## 前置环境：Xtensa Rust 工具链

构建界面需要 Espressif 的 Xtensa Rust 工具链（esp-rs 的 rustc 分支）与它自带的 `rust-src` 组件（`-Zbuild-std=core,alloc` 要用）。
本仓库验证过的组合：espup 0.17.1 + rustup 里的 `esp` 工具链（rustc 1.97.0-nightly 8ea53bcd7 2026-07-08，LLVM 21.1.3）。

换机器先跑脚本，缺什么装什么，已装好只打印版本：

```powershell
cd <仓库根>
uv run python scripts/setup-rust-toolchain.py          # 没有 espup 时自动下载它的预编译二进制
uv run python scripts/setup-rust-toolchain.py --check  # 只检查，不改动环境
```

工具链装在别的名字下时加 `--toolchain <名字>`，取值要与下面的 CMake 变量一致；脚本会顺带检查 `rust-src` 与 `xtensa-esp32s3-none-elf` 目标。

手工装按下面步骤（Windows PowerShell，脚本跑不通或必须离线安装时）：

```powershell
# 1. 先装 rustup（https://rustup.rs），espup 用它管理工具链
# 2. 装 espup：GitHub Releases 的预编译二进制，或
cargo install espup --locked
# 3. 装 Xtensa 工具链（只装 esp32s3 目标，落在 ~/.rustup/toolchains/esp）
espup install -t esp32s3
# 4. 验证：两条都能打印版本即可
rustc +esp --version
cargo +esp --version
# 5. 目标三元组可用：能打印 cfg 说明整套工具链齐了
rustc +esp --target xtensa-esp32s3-none-elf --print cfg
```

工具链名不是 `esp` 时见下面「CMake 变量」的 `REMAPAD_SLINT_RUST_TOOLCHAIN`；
缺 `rust-src` 时用 `rustup component add rust-src --toolchain <名字>` 补。

`espup install` 会在用户目录生成 `export-esp.ps1`，把工具链里的 gcc/clang 加进 PATH。
本仓库的构建只用到 rustc 与 rust-src（只产出静态库、不链接），日常 `idf.py build` 不需要先执行这个脚本。

固定版本用 `espup install -t esp32s3 -v <esp-rs rust-build 版本>`（对应 `espup install --help` 的 `--toolchain-version`）；
不带版本号装的是最新版，升级工具链后先确认 `idf.py build` 能过再提交。

## CMake 变量

| 变量 | 默认值 | 作用 |
| :--- | :--- | :--- |
| `REMAPAD_UI` | `ON` | 是否编入屏幕 UI；OFF 时纯 C 构建固件（不需要 Rust 工具链，屏幕熄灭、设置走串口 CLI） |
| `REMAPAD_SLINT_RUST_TOOLCHAIN` | `esp` | 构建时调用 `cargo +<名字>`；工具链换了名字，或本机留了多套 esp 工具链时改它 |
| `REMAPAD_SLINT_FONT` | `ui/assets/fonts/NotoSansSC-Regular.otf` | 构建期烘字形用的字体文件 |

`REMAPAD_SLINT_RUST_TOOLCHAIN` 有两种给法，都在 CMake 配置阶段读取，环境变量优先：

```powershell
cd firmware
# 临时切换：改完重跑配置（配置阶段会打印实际取用的名字）；环境变量撤销后回到缓存值
$env:REMAPAD_SLINT_RUST_TOOLCHAIN = "my-esp"
idf.py reconfigure
# 持久切换：写进 CMake 缓存，往后的构建都用它
idf.py -DREMAPAD_SLINT_RUST_TOOLCHAIN=my-esp build
```

工具链名留空、写成不存在的名字或缺 rust-src 时，配置阶段会直接停下并打印修复命令；
改完环境变量记得先 `idf.py reconfigure`，否则构建仍按上次配置取的名字走。

## 常用命令

```powershell
# 整机编译：界面编译与静态库都由它触发，中间产物在 firmware/build/esp-idf/slint_ui/cargo/ 下
cd firmware ; idf.py build

# 只编界面与 Rust 组件，几秒出结果（产物落在 ui/target/，已忽略）
# 这条通路用 build.rs 的默认值：节拍率 1000 与仓库字体，与 idf.py build 传入取值一致时才等价
cd ui/slint_ui
cargo +esp build -Zbuild-std=core,alloc --release --locked --target xtensa-esp32s3-none-elf

# 只重烧应用分区
cd firmware ; idf.py -p COMx app-flash
```

## 改动注意

- 文案写在 `.slint` 里，不要在固件侧拼中文（见上文烘焙规则）。
- 字号只取 `theme.slint` 里已有的档位；新增字号要同步 `build-support` 的 `FONT_SIZES`，否则静默退回最近的一档位图。
- 单色图标用字形：`Icon { glyph: "\u{e30c}"; size: ...; tint: ...; }`，码点从下面这份对照表取（都是 Material Symbols 的现成字形）。
  新增图标只改界面与码点即可。图标字号只能取烘过的档位（12/14/16/24）。

  | 码点 | 字形 | 用在哪 |
  | :--- | :--- | :--- |
  | U+E5CB / U+E5CC | chevron_left / chevron_right | 卡片左右两侧的翻页箭头 |
  | U+E30C / U+E99D | desktop_windows / desktop_access_disabled | 底栏 USB 模式格（PC 已连 / 未连） |
  | U+E338 / U+E500 | videogame_asset / videogame_asset_off | 底栏 USB 模式格（手柄已插 / 未插）、模式页手柄卡片 |
  | U+E701 | missing_controller | 底栏主机连接格 |
  | U+F30D U+F30C U+F30B U+F30A U+F309 U+F308 U+F307 U+F304 | battery_android_0 … _6 与 full | 底栏电量格：每 15% 一档，100% 给满格（15% 以下整体转错误色） |
  | U+EECB U+EECA U+EEDE U+EEDB | gamepad 方向与肩键 | 底栏手柄提示行的「翻页」 |
  | U+EEC9 U+EECC | gamepad 上下 | 底栏手柄提示行的「选择」 |
  | U+EECE / U+EED0 | gamepad_circle_right / _down | 底栏手柄提示行的「确认 / 退出」 |
  | U+2801 / U+2802 / U+2804 / U+2840 / U+2880 / U+2820 / U+2810 / U+2808 | 盲文点阵八帧 | 配对页状态行的转圈 |
  不要退回 `@image-url("x.svg")`：SVG 按固有尺寸在构建期光栅化，缩到显示尺寸上屏会糊。
- 焦点环可见窗口、切页与拖动的交互约定写在 `src/app.slint` 的注释里；重绘代价规则同样是改界面时的硬约束。
  弹窗打开时焦点环只在弹窗的按钮上：页面按 `page-focus`（操控窗口打开且没有弹窗）决定要不要画环。
- 平台层按单线程前提实现（Slint 的 `unsafe-single-threaded`）：界面状态与动作只在 UI 任务上访问，
  固件侧要用状态轮询与原子标记交接，不能从别的任务直接调进来。

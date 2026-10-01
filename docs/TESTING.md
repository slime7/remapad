# Remapad 测试策略与回归规则

本项目测试由四部分组成：屏幕 UI 宿主用例、屏幕 UI 浏览器端到端用例、固件主机端单元测试与 PC 侧单元测试。全部在开发机运行。

## 屏幕 UI 宿主用例

测试对象为开发机编译的真实界面产物，通过注入状态并断言元素几何与像素墨迹。

### 运行

```powershell
cargo test --locked --manifest-path ui/Cargo.toml                  # 全量
cargo test --manifest-path ui/Cargo.toml --test bottom_bar 底栏    # 只跑匹配的用例
```

### 断言口径

- **几何尺寸**：通过 `ui::rect()` 获取元素绝对位置与尺寸。
- **像素墨迹**：通过 `ui::frame()` 结合 `count_color` / `ink_bounds` 检查真实渲染落色。
- **指针事件**：使用真指针事件打入点按与拖动手势。

### 覆盖范围

| 用例 | 覆盖的行为 |
| :--- | :--- |
| [ui/host/tests/bottom_bar.rs](../ui/host/tests/bottom_bar.rs) | 三等分状态格的格心与图标/标签同轴居中、手柄操控提示行的对齐、OTA 进度条居中且从条槽左端起填充 |
| [ui/host/tests/pairing_page.rs](../ui/host/tests/pairing_page.rs) | 转圈按相位轮换盲文点阵单点、状态行在有无转圈时都居中 |
| [ui/host/tests/system_page.rs](../ui/host/tests/system_page.rs) | 电池行的中点分隔符画成小圆点（字符集锚点漏码点就会红） |
| [ui/host/tests/pages.rs](../ui/host/tests/pages.rs) | 调试页画在末位槽号上、切页后只画当前页与左右邻页、整条带按一页步距滑行再回到静止位置、静止时两侧各露一条花瓣边、静止画面以两百像素为周期 |
| [ui/host/tests/bottom_bar.rs](../ui/host/tests/bottom_bar.rs) | 底栏电量图标按电量逐档变满（0-6 档加满格共八个字形） |
| [ui/host/tests/bands.rs](../ui/host/tests/bands.rs) | 行带计划：整屏按 48 行切分与末段余数、不满宽与越界矩形的裁剪、逐行取像素的步长（真源码在 ui/render-plan，固件平台层跑同一份） |
| [ui/host/tests/gestures.rs](../ui/host/tests/gestures.rs) | 拖动时条带到切页阈值就定住、越过阈值抬手翻到上一页并接着整页滑行、没过阈值不翻页并回弹、单步甩动即翻页、往回滑取消换页、滑行期间拖动让位 |
| [ui/host/tests/dialogs.rs](../ui/host/tests/dialogs.rs) | 弹窗遮罩盖住整屏并压暗、弹窗期间页面控件收不到点按、页面焦点环让位给弹窗、重启与关机等待画面盖住整屏 |
| [ui/host/tests/network_page.rs](../ui/host/tests/network_page.rs) | 无线调试页 WiFi 开关在右上角且点按发出开关动作、左上角信号图标随 RSSI 分档（关闭字形兜底）、正文随会话状态切换 |

## 屏幕 UI 浏览器端到端用例

基于 Playwright 驱动 Chromium 运行 WASM 预览页，验证浏览器加载、Canvas 像素渲染、指针输入及 HTML 控制台交互。

### 运行

```powershell
cd ui/preview
pnpm install                      # 首次装 @playwright/test
pnpm exec playwright install chromium   # 首次装浏览器
node tools/build.mjs              # 首次构建预览产物（日常改动由 pnpm dev 的监听自动重编）
pnpm test                         # webServer 自动拉起静态服务并跑全量
pnpm exec playwright test pointer # 只跑文件名/用例名匹配的用例
```

### 覆盖范围

| 文件 | 覆盖的行为 |
| :--- | :--- |
| `e2e/smoke.spec.mjs` | 预览页装载、canvas 元素 240 × 280、玻璃面 8px 黑边、快照默认值、首帧画出亮度页、125% 小数缩放下照常启动 |
| `e2e/actions.spec.mjs` | 动作注入按固件语义结算：亮度钳位、翻页回绕、弹窗压暗与确认键、重启/关机遮罩与自动收起、无线调试开关、主机格空闲守卫、焦点循环与无可聚焦项报告 |
| `e2e/pointer.spec.mjs` | 真指针事件走 canvas：点按亮度角钮、拖动整页滑行、点底栏主机格 |
| `e2e/layout.spec.mjs` | 物理屏外观（画布 40px / 外框 48px 圆角、8px 玻璃黑边、弧内外命中让位）与响应式布局（宽屏左右、窄屏上下） |
| `e2e/console.spec.mjs` | HTML 控制台：动作按钮结算、模拟状态直写、标签与高亮随快照回填、设备画面点按回写控制台读数 |

### 断言口径

- **快照状态**：`remapad.snapshot()` 校验模拟状态与属性。
- **像素采样**：Canvas 特定坐标采样比对语义颜色，不做整屏图像比对。
- **控制台回显**：校验动作日志输出与状态反馈。

## 用例纪律

- **先写用例再改代码**：修改前先补充失败用例，验证转绿后方算完成。
- **优先端到端断言**：交互与画面行为优先在宿主/端到端层测试，单元测试专注纯逻辑。
- **使用真实源码**：测试直接编译被测源码，桩代码仅用于模拟硬件外设与操作系统头文件。

## 固件主机端单元测试

### 被测范围

| 模块 | 测试内容 |
| :--- | :--- |
| `main/target/ns2/ns2_report.c` | 报告位图、摇杆数据打包与字节序校验 |
| `main/target/ns2/ns2_serial.c` | 序列号与地址派生校验 |
| `main/target/ns2/ns2_frames.c` | 命令帧格式与公钥参数校验 |
| `main/target/ns2/ns2_identity.c` | 身份枚举与地址派生 |
| `main/target/ns2/ns2_upgrade.c` | 固件更新记录流装配与应答伪装 |
| `main/pad/pad_device.c` 与 `layouts/` | 家族布局表偏移、按键位置映射、轴量程与死区 |
| `main/input/input_frame.c` | 桥接帧 CRC 校验与重同步 |
| `main/ota/ota_proto.c` | OTA 序号判定、窗口流控应答与聚合 |
| `main/target/ns2/ns2_target.c` | 私有格式到目标报文映射 |
| `main/dp/dp_source.c` | 多源输入叠加与调试注入分流 |
| `main/dp/dp_capture.c` | 原始输出采集入队与截断丢包处理 |
| `main/dp/dp_ui.c` | 组合键捕获与屏幕控制按键映射 |
| `main/dp/dp_power.c` | 省电状态判据与节拍分频计算 |
| `main/target/ns2/ns2_adv.c` | 广播载荷组装与唤醒窗口决策 |
| `main/ui/ui_service.c` | UI 状态快照填充与控制面动作分发 |
| `main/netlog/netlog_retry.c` | 网络重连退避策略与失败判定 |

不在这套测试里：面板/触摸/背光驱动、BLE 与 NVS（含 BLE 栈的起停：控制器关断与重新起栈）、USB host、
桥接链路的串口驱动与接收任务、启动画面与 UI 任务调度——它们依赖真实硬件时序与协议栈，只能在真机上验证。

### 目录

| 文件 | 作用 |
| :--- | :--- |
| [scripts/firmware-test.py](../scripts/firmware-test.py) | 编译并运行：探测编译器、编译被测源码与用例、跑可执行文件 |
| `firmware/test/support/host_test.h` / `.c` | 断言宏与运行器（几十行，够用即可） |
| `firmware/test/suites.c` | 套件注册表 |
| `firmware/test/support/stubs/` | 只在主机编译时生效的 ESP-IDF 最小替身 |
| `firmware/test/test_*.c` | 用例 |

用例编译的是**固件里的真源码**，不是副本；
替身只补 `esp_err.h`、`esp_log.h`、FreeRTOS 临界区宏这类环境头文件，`app_config` 的取值入口，
主机上没有的 `heap_caps_*` 分配接口（NS2 输出封装因此能整段进测试），以及 ui_service 用到的硬件状态源
（电池/配对/USB/OTA 读数，见 `stubs/ui_service_deps_stub.c`）。
被替换的都是硬件相关实现，编码与像素逻辑一行都没有复制。

### 运行

```powershell
uv run python scripts/firmware-test.py
```

编译器按 `CC` 环境变量、MSVC（自动探测 `vcvars64.bat`）、`clang`、`gcc` 的顺序探测，`CC=clang` 可以强制指定。
构建产物写在 `firmware/build/host-tests/`（已被 Git 忽略）；失败时按文件:行号给出断言位置与实际值。

### 新增用例

1. 在 `firmware/test/test_<模块>.c` 里加一个函数与一条 `HOST_TEST_SUITE` 表项；
2. 新文件要在 `firmware/test/suites.c` 注册，并在 `scripts/firmware-test.py` 的 `TEST_SOURCES` 里列出来；
3. 断言用 `CHECK` / `REQUIRE` / `CHECK_EQ` / `CHECK_BYTES`；定长报文优先用 `CHECK_BYTES` 写黄金样本，任何一位错位都会指名道姓地报出第一个不同的字节。

注意 `dp_source` 的源注册表是进程级静态状态，同一个文件里的用例按注册顺序相互影响，新增用例不要假设注册表是空的。

## PC 侧主机端用例

基于 Python 标准库 `unittest` 测试 PC 侧工具纯逻辑：

| 文件 | 测试内容 |
| :--- | :--- |
| `pc/tests/test_ports.py` | 串口端口枚举与异常提示 |
| `pc/tests/test_image.py` | 固件镜像头结构与描述符校验 |
| `pc/tests/test_frame_codec.py` | 桥接帧编码、解码与 CRC 校验 |
| `pc/tests/test_pick_device.py` | 手柄接口过滤与设备匹配 |
| `pc/tests/test_session_output.py` | 输出分流与命令行解析 |
| `pc/tests/test_settings_reply.py` | 设备状态回读格式解析 |
| `pc/tests/test_mcp_pad.py` | MCP 按键服务：键位表、快照报文合成、引擎帧输出与脚本时间线 |

```powershell
uv run python -m unittest discover -s pc/tests -t pc     # 在仓库根执行
uv run python -m unittest discover -s pc/tests -t pc -v  # 加 -v 看每条用例名
```

用例跑的是 `pc/` 下的真源码（`import ctrl` / `import link`），不复制被测逻辑，也不创建窗口：
界面本身靠实机与隐藏窗口的手工走查，只有它的队列接收器（`gui.QueueReporter`）与输入框取值、时长格式这类纯函数进用例。
新增用例直接放进 `pc/tests/`，文件名以 `test_` 开头。

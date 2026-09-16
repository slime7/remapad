# Remapad 测试策略与回归规则

本项目用自动化测试守住行为：**UI 端到端测试**（Playwright 驱动触摸预览页里的真实产物）、**固件主机端单元测试**（把与硬件无关的纯逻辑模块编译成 PC 可执行文件）与 **PC 侧主机端用例**（`pc/` 工具的纯逻辑，标准库 unittest）。三者都跑在开发机上，不需要真机。

## UI 端到端测试

### 为什么是「预览页 + 真产物」

测试对象不是另写一套模拟环境，而是 `pnpm run dev` 用的同一个触摸预览页：

- 页面加载 `ui/dist/` 里的真实 `.js` 与 `.pak`，由官方编译器从 `ui/src` 构建；
- 渲染核心是官方 wasm，与真机同一条布局与光栅化路径，像素结果确定性一致；
- 输入只有触摸屏一种，测试台把指针事件换成设备触点，与真机同一套手势与命中判定。

因此「预览页里点得到、画得出」与「真机上点得到、画得出」是同一件事；帧率与内存占用另算。

### 目录

| 文件 | 作用 |
| :--- | :--- |
| [ui/playwright.config.ts](../ui/playwright.config.ts) | 配置：串行执行、启动预览服务器（与 `pnpm run dev` 同一条命令）、失败留痕 |
| [ui/tests/e2e/fixtures.ts](../ui/tests/e2e/fixtures.ts) | 测试台：触摸驱动、组件树、像素、读数面板 |
| [ui/tests/e2e/harness.ts](../ui/tests/e2e/harness.ts) | 页面侧探针：接住官方 DevTools 通道，提供组件树与边界命中 |
| [ui/tests/e2e/pages.ts](../ui/tests/e2e/pages.ts) | 页面级操作：从默认状态走到某个功能页 |
| `ui/tests/e2e/*.spec.ts` | 用例：启动、页面切换、滚动、手柄设置、配对、系统、手柄操控屏幕 |

### 运行

```powershell
pnpm run test:e2e                    # 全量跑一遍
pnpm run test:e2e:headed             # 打开浏览器看过程
pnpm exec playwright test -g 滚动     # 只跑匹配的用例（在 ui/ 目录下执行）
```

命令会先按 `pnpm run dev` 的方式编译产物并拉起预览服务器（8130）。本地已有 dev 会话时直接复用，不会重复启动。

### 三层事实来源

断言越靠前越稳：

1. **组件树**（`app.nodes()` / `app.visibleTexts()`）——官方节点树镜像，含文本、class 与 hidden 传播结果。只改样式、不改行为时不会误报。
2. **屏幕像素**：`app.colorAt()` / `app.regionSignature()` / `app.colorShare()` / `app.brightShare(rect, threshold)`。
  这一层看视觉与位移类事实。区域指纹只回答「这块画面变没变、变回去了没有」，不把整幅截图当基线；`brightShare` 数区域内「亮到发白」的像素占比，用来判 2px 焦点环这类抗锯齿描边（按精确色判会漏掉大半）。
3. **预览页读数**（`app.readout()`）——上屏状态、帧率、触点与命中节点，用来确认宿主侧链路本身正常。

坐标一律使用**设备逻辑像素**（240 × 280 视口）：`app.touch` 按画布实际显示尺寸换算，预览页的缩放开关不影响用例。

### 操作与定位

- `app.touch.tap(x, y)`：点击。
- `app.touch.drag(from, to, { steps, dwellMs })`：逐帧跟随的慢速拖动，松手速度接近 0，用于精确落点。
- `app.touch.flick(from, to)`：整段位移在一两帧内走完并立刻抬手，用于触发惯性滚动。**抬手必须紧跟最后一次移动**：中间等帧会把触点速度采样成 0，手势层只会停在手指位置，不产生甩动。
- `app.tapText('开始')`：按文本定位并点击。定位用官方边界命中（与触摸按下同一条判定）扫描「节点自己 → 最近的祖先 → 后代」，所以「定位到」就等于「点得到」，且不受增量重绘影响。
- `app.pad.press('ArrowLeft')` / `app.pad.pressTimes('ArrowLeft', 3)`：手柄按键驱动。
  预览页把方向键 / WASD 当十字键位、回车 / 空格当圆圈键位（与真机同一份按键位契约，见 [ADR 0028](adr/0028-pad-combo-captures-screen.md)）。
  每次按下至少跨一帧应用才看得到，连按是逐次独立的下沿。

### 三个已知坑

1. **官方 `inspect` 会给整幅画面加调试着色**（像素整体变亮，`inspect(0)` 之前不消失）。`app.inspectRect()` 读完矩形会立刻清掉并等两帧重绘；
   连续采样请用 `app.sampleScroll()`。
2. **`inspect` 只在节点被重绘的那一帧拿得到矩形**，静止且无重绘时它会超时返回 `null`。需要位置时优先用 `app.tapText()` 的边界命中。
3. **状态栏每秒更新一次运行时长**：像素指纹的区域要避开顶部 26 px，用例里统一从 y=34 起采样。

## 缺陷修复必须先有用例

**修 UI bug 的流程是：先加一条能复现的 E2E 用例（此时必须是红的），再改 `ui/src`，用例转绿才算修完。** 提交信息里带上用例名。固件里与硬件无关的逻辑缺陷（编码、校验、合成、像素）同理，先补主机端用例。
PC 侧工具里与设备无关的逻辑缺陷（串口枚举、镜像校验、帧编解码、工具命令解析）同样先补 `pc/tests/` 的用例。

这条规则的用意是让每个修过的 bug 都留下一条可重复执行的证据：没有用例的修复无法证明问题真的消失，也无法保证下次重构不再犯。具体要求：

- 用例标题写「用户看到的现象」，不写实现细节（例如「甩动到边界不越界不回弹」，而不是「scroller 改用 tween」）。
- 断言要能真的失败：写完用例后先确认它在未修复的版本上失败，再动手改代码。
- 不要为了让用例通过而放宽断言或删掉用例。行为确实变了就改断言，理由写进提交信息。
- 同一个行为只保留一条用例。发现重复时合并，不新增。

## 固件主机端单元测试

### 为什么在开发机上跑

ESP-IDF 自带的 Unity 要烧到真板上、经串口收结果，改一行也要等一次烧录；而这批逻辑缺陷（编码位错、校验位算错、像素合成差一档）与硬件无关，编译成开发机上的可执行文件几秒钟就能跑完，可以放进每次改动的必跑清单。
真机仍然是必要的验收环节，但不再是第一道关。

### 被测范围

| 模块 | 为什么值得测 |
| :--- | :--- |
| `main/target/ns2/ns2_report.c` | 报告位图错一位就是某个按键失灵或摇杆偏移，真机上很难定位 |
| `main/target/ns2/ns2_serial.c` | 序列号校验位决定主机是否认这台手柄 |
| `main/target/ns2/ns2_frames.c` | 命令帧固定字段与配对公钥，写错就整条命令通路静默失效 |
| `main/target/ns2/ns2_identity.c` | 身份短名与对外地址派生，左右两只派生同址会被主机看成同一只手柄 |
| `main/target/ns2/ns2_upgrade.c` | 主机手柄固件更新的记录流装配（记录头、帧首/续帧、帧长判定）错了会表现为「更新推一半停住」或伪装应答答错帧；用例用实机抓包字节钉住首帧 4108 字节的装配结果 |
| `main/pad/pad_device.c` 与 `main/pad/layouts/` | 家族布局表的偏移错了会表现为「按 A 出了 B」或摇杆漂移，真机上只能靠猜；按键位置映射、量程归一、死区、按 PID 分行与未知型号兜底在这里钉住 |
| `main/input/input_frame.c` | 帧校验与失步重同步写错会表现为「手柄偶尔失灵」或命令行冒出乱码，两种现象都难复现 |
| `main/ota/ota_proto.c` | 升级序号判定、窗口应答、4 KB 聚合与超时写错会表现为「升级卡住」「写坏镜像」或「PC 以为成功而设备没换分区」；载荷布局与应答字节在这里逐条钉住（`ota_session` 依赖 `esp_ota`，不进这套测试） |
| `main/target/ns2/ns2_target.c` 与 `ns2_output.c` | 私有格式到 NS2 报文的映射（面键位置、背键折并 GL/GR、扳机 50% 阈值、电量折进报告）错了就是实机上「按键对不上」；测试驱动真实编码路径断言报文字节 |
| `main/render_accel.c` | 本机像素回调必须与软件路径逐像素一致，差一档就是色带或错行 |
| `main/dp/dp_source.c` | 多路输入叠加规则错了会表现为摇杆漂移、注入按键卡住；按键名表与摇杆注入的分侧语义也在这里钉住 |
| `main/dp/dp_ui.c` | 组合键捕获的判定错了会表现为「按住组合键没反应」或普通按键被吞掉，真机上不好复现；四键同按、300 ms 阈值与十字键 / 圆圈键到官方按键位的映射在这里钉住 |

不在这套测试里：面板/触摸/背光驱动、BLE 与 NVS、USB host、桥接链路的串口驱动与接收任务、启动画面与 owner task 调度——它们依赖真实硬件时序与协议栈，只能在真机上验证。

### 目录

| 文件 | 作用 |
| :--- | :--- |
| [scripts/firmware-test.mjs](../scripts/firmware-test.mjs) | 编译并运行：探测编译器、编译被测源码与用例、跑可执行文件 |
| `firmware/test/support/host_test.h` / `.c` | 断言宏与运行器（几十行，够用即可） |
| `firmware/test/suites.c` | 套件注册表 |
| `firmware/test/support/stubs/` | 只在主机编译时生效的 ESP-IDF 最小替身 |
| `firmware/test/test_*.c` | 用例 |

用例编译的是**固件里的真源码**，不是副本；
替身只补 `esp_err.h`、`esp_log.h`、FreeRTOS 临界区宏这类环境头文件，`app_config` 的取值入口，以及主机上没有的 `heap_caps_*` 分配接口（NS2 输出封装因此能整段进测试）。
被替换的都是硬件相关实现，编码与像素逻辑一行都没有复制。

### 运行

```powershell
pnpm run test:firmware
```

编译器按 `CC` 环境变量、MSVC（自动探测 `vcvars64.bat`）、`clang`、`gcc` 的顺序探测，`CC=clang` 可以强制指定。
构建产物写在 `firmware/build/host-tests/`（已被 Git 忽略）；失败时按文件:行号给出断言位置与实际值。

### 新增用例

1. 在 `firmware/test/test_<模块>.c` 里加一个函数与一条 `HOST_TEST_SUITE` 表项；
2. 新文件要在 `firmware/test/suites.c` 注册，并在 `scripts/firmware-test.mjs` 的 `TEST_SOURCES` 里列出来；
3. 断言用 `CHECK` / `REQUIRE` / `CHECK_EQ` / `CHECK_BYTES`；定长报文优先用 `CHECK_BYTES` 写黄金样本，任何一位错位都会指名道姓地报出第一个不同的字节。

注意 `dp_source` 的源注册表是进程级静态状态，同一个文件里的用例按注册顺序相互影响，新增用例不要假设注册表是空的。

## PC 侧主机端用例

`pc/` 下的工具也有与设备无关的纯逻辑（串口枚举、镜像校验、帧编解码、输出分流、工具命令解析），
这部分跑标准库 `unittest`，不需要板子：

| 文件 | 为什么值得测 |
| :--- | :--- |
| `pc/tests/test_ports.py` | 串口枚举与打开失败的提示文案错了会把用户引向错误的排查方向 |
| `pc/tests/test_image.py` | 升级镜像的本地校验条件决定「推了个不是本设备的镜像」会不会被拦下，文案要指明是哪一项不合格 |
| `pc/tests/test_frame_codec.py` | 桥接帧的成帧、CRC 拒绝与失步重同步错一位会表现为「手柄偶尔失灵」或命令行冒出乱码 |
| `pc/tests/test_pick_device.py` | 候选接口的用途过滤与 VID/PID/接口路径选择错了会表现为「转发到别的手柄」 |
| `pc/tests/test_session_output.py` | 输出分流（命令行写标准流、界面写队列）与工具命令解析错了会表现为日志缺失或报错位置错乱 |

```powershell
pnpm run test:pc                                            # 仓库根
cd pc ; uv run python -m unittest discover -s tests -t . -v  # 单独跑
```

用例跑的是 `pc/` 下的真源码（`import remapadctl` / `import link`），不复制被测逻辑，也不创建窗口：
界面本身靠实机与隐藏窗口的手工走查，只有它的队列接收器（`remapadgui.QueueReporter`）进用例。
新增用例直接放进 `pc/tests/`，文件名以 `test_` 开头。

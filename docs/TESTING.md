# Remapad 测试策略与回归规则

本项目用自动化测试守住行为：**屏幕 UI 宿主用例**（界面的 `#[test]`，编译真实界面产物并渲染一帧）、
**固件主机端单元测试**（把与硬件无关的纯逻辑模块编译成 PC 可执行文件）与
**PC 侧主机端用例**（`pc/` 工具的纯逻辑，标准库 unittest）。三套都跑在开发机上，不需要真机。

## 屏幕 UI 宿主用例

`ui/host/tests/*.rs` 是界面的 `#[test]` 用例：同一份 `ui/src/*.slint` 与 `ui/preview.slint` 在开发机上编译，
装界面测试后端注入界面状态，再用软件渲染器渲染一帧，按**元素几何**与**画面像素**断言。
被测对象是真实产物（同一套字体烘焙、固件里同一个软件渲染器），不需要真机与串口。

### 运行

```powershell
cargo test --locked --manifest-path ui/Cargo.toml                  # 全量
cargo test --manifest-path ui/Cargo.toml --test bottom_bar 底栏    # 只跑匹配的用例
```

窗口固定按面板尺寸（240 × 280）渲染，只需要宿主 stable 工具链；xtensa 工具链只服务于固件构建。
界面里的 id 是用例的查询入口（例如 `BottomBar::battery-text`），改 id 要同步改 `ui/host/tests/`。

### 断言口径

- **布局事实用元素几何**：`ui::rect()` 拿元素相对窗口的绝对位置与尺寸；
- **「画出来了没有」用画面墨迹**：`ui::frame()` 渲染一帧，`count_color` / `ink_bounds` / `ink_runs` 按区域找落色与墨迹分段。
  只出现在运行期字符串里的码点不会被烘成字形，上屏是空洞或豆腐块——这类缺陷只有像素层看得见（见 [ui/README.md](../ui/README.md)）；
  - 屏幕坐标是设计稿量测值（底栏 8..232、三格按整屏 240 均分、图标与标签的中线等），换设计稿要同步改断言；
  - **手势用真指针事件**：`ui::press` / `move_to` / `release` / `tap` 按窗口坐标打成对的指针事件，拖动与点按都落在真控件上；
  - 元素查询只认当前画出来的元素：`visible: false` 的页与没实例化的 `if` 块都查不到，切页后再定位要复用切页前记下的盒子。

### 覆盖范围

| 用例 | 覆盖的行为 |
| :--- | :--- |
| [ui/host/tests/bottom_bar.rs](../ui/host/tests/bottom_bar.rs) | 三等分状态格的格心与图标/标签同轴居中、手柄操控提示行的对齐、OTA 进度条居中且从条槽左端起填充 |
| [ui/host/tests/pairing_page.rs](../ui/host/tests/pairing_page.rs) | 转圈按相位轮换盲文点阵单点、状态行在有无转圈时都居中 |
| [ui/host/tests/system_page.rs](../ui/host/tests/system_page.rs) | 电池行的中点分隔符画成小圆点（字符集锚点漏码点就会红） |
| [ui/host/tests/pages.rs](../ui/host/tests/pages.rs) | 调试页画在末位槽号上、切页后只画当前页、翻页时卡片从行进侧滑入再回到静止位置 |
| [ui/host/tests/bottom_bar.rs](../ui/host/tests/bottom_bar.rs) | 底栏电量图标按电量逐档变满（0-6 档加满格共八个字形） |
| [ui/host/tests/preview.rs](../ui/host/tests/preview.rs) | 预览窗控制条翻页后设备画面切到下一张卡片、确认键走设备上的焦点分发、焦点到底再按循环到另一端（PC 预览的交互靠它守住） |
| [ui/host/tests/bands.rs](../ui/host/tests/bands.rs) | 行带计划：整屏按 48 行切分与末段余数、不满宽与越界矩形的裁剪、逐行取像素的步长（真源码在 ui/render-plan，固件平台层跑同一份） |
| [ui/host/tests/gestures.rs](../ui/host/tests/gestures.rs) | 拖动时卡片按位移的一半跟手且封顶 24px、向右拖过阈值抬手翻到上一页、没过阈值不翻页并回弹、单步甩动即翻页 |
| [ui/host/tests/dialogs.rs](../ui/host/tests/dialogs.rs) | 弹窗遮罩盖住整屏并压暗、弹窗期间页面控件收不到点按、页面焦点环让位给弹窗、重启与关机等待画面盖住整屏 |

## 用例纪律

- **先写用例再改代码**：改屏幕 UI 的 bug 先在 `ui/host/tests/` 加一条能复现的红用例（宿主侧跑真实产物），改完让用例转绿才算修完；
  固件里与硬件无关的逻辑缺陷先补 `firmware/test/` 的主机端用例，PC 侧工具同理先补 `pc/tests/` 的用例。任何业务改动都一样：
  没红过不许改代码，没转绿不算改完。用例标题写用户看到的现象，不放宽断言迁就实现，同一行为只留一条用例。
- **优先端到端测试**：能在端到端层断言的行为（屏幕画面与交互、真实产物链路）就在端到端层写；单元用例只补端到端覆盖不到的纯逻辑。
- **开发期间不跑端到端套件**：迭代中只跑当前这条用例，端到端套件等全部改完再整跑一次；各套用例的位置与运行方式见下面各节。
- **测试只用真源码**：屏幕 UI 用例编译 `ui/src/` 真实界面产物并在软件渲染器上渲染画面，固件测试编译 `firmware/main/` 下的源码；
  `firmware/test/support/stubs/` 只补齐主机缺失的 ESP-IDF 头文件与硬件取值入口，不得把被测逻辑复制一份进测试。

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
| `main/pad/pad_device.c` 与 `main/pad/layouts/` | 家族布局表的偏移错了会表现为「按 A 出了 B」或摇杆漂移，真机上只能靠猜；按键位置映射、量程归一、死区、运动刻度换算、按 PID 分行与未知型号兜底在这里钉住 |
| `main/input/input_frame.c` | 帧校验与失步重同步写错会表现为「手柄偶尔失灵」或命令行冒出乱码，两种现象都难复现 |
| `main/ota/ota_proto.c` | 升级序号判定、窗口应答、4 KB 聚合与超时写错会表现为「升级卡住」「写坏镜像」或「PC 以为成功而设备没换分区」；载荷布局与应答字节在这里逐条钉住（`ota_session` 依赖 `esp_ota`，不进这套测试） |
| `main/target/ns2/ns2_target.c` 与 `ns2_output.c` | 私有格式到 NS2 报文的映射（面键位置、背键折并 GL/GR、扳机 50% 阈值、电量折进报告）错了就是实机上「按键对不上」；测试驱动真实编码路径断言报文字节 |
| `main/dp/dp_source.c` | 多路输入叠加规则错了会表现为摇杆漂移、注入按键卡住；按键名表与摇杆注入的分侧语义也在这里钉住 |
| `main/dp/dp_capture.c` | 主机原始输出采集的入环/出队写错会表现为 PC 抓包缺包乱序、长块尾巴静默丢失；排队次序、截断标记、满队丢包计数与开关清理在这里钉住 |
| `main/dp/dp_ui.c` | 组合键捕获的判定错了会表现为「按住组合键没反应」或普通按键被吞掉，真机上不好复现；四键同按、300 ms 阈值与十字键 / 圆圈键到官方按键位的映射在这里钉住 |
| `main/dp/dp_power.c` | 省电档判据与节拍换算错了会表现为「BLE 已关闭却还按 5 ms 采样」或上报分频为 0 而永远不发报告；BLE 栈关闭即省电档、83 ms 节拍与分频下限在这里钉住 |
| `main/target/ns2/ns2_adv.c` | 广播载荷与策略错了会表现为主机发现不了、唤不醒或回连不上；三种形态的字节、窗口决策与「完全静默才允许关 BLE 控制器」的判据在这里钉住 |
| `main/ui/ui_service.c` | UI 契约的动作映射与状态装配错了会表现为「按钮按了没反应」「提示语不对」或系统页读数错；亮度档位、动作到控制面命令的 JSON、提示/弹窗流转与快照逐字段映射在这里钉住（状态源走替身） |

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

`pc/` 下的工具也有与设备无关的纯逻辑（串口枚举、镜像校验、帧编解码、输出分流、工具命令解析、设置回读行解析），
这部分跑标准库 `unittest`，不需要板子：

| 文件 | 为什么值得测 |
| :--- | :--- |
| `pc/tests/test_ports.py` | 串口枚举与打开失败的提示文案错了会把用户引向错误的排查方向 |
| `pc/tests/test_image.py` | 升级镜像的本地校验条件决定「推了个不是本设备的镜像」会不会被拦下，文案要指明是哪一项不合格 |
| `pc/tests/test_frame_codec.py` | 桥接帧的成帧、CRC 拒绝与失步重同步错一位会表现为「手柄偶尔失灵」或命令行冒出乱码 |
| `pc/tests/test_pick_device.py` | 候选接口的用途过滤与 VID/PID/接口路径选择错了会表现为「转发到别的手柄」 |
| `pc/tests/test_session_output.py` | 输出分流（命令行写标准流、界面写队列）与工具命令解析错了会表现为日志缺失或报错位置错乱 |
| `pc/tests/test_settings_reply.py` | 设置回读行的解析错了会表现为界面显示的亮度、配色或开关与设备不一致（固件是唯一事实源，界面只跟回读走），接上设备之前看不出来 |

```powershell
cd pc ; uv run python -m unittest discover -s tests -t .     # 仓库根或 pc/ 下都是这条
cd pc ; uv run python -m unittest discover -s tests -t . -v  # 加 -v 看每条用例名
```

用例跑的是 `pc/` 下的真源码（`import remapadctl` / `import link`），不复制被测逻辑，也不创建窗口：
界面本身靠实机与隐藏窗口的手工走查，只有它的队列接收器（`remapadgui.QueueReporter`）与输入框取值、时长格式这类纯函数进用例。
新增用例直接放进 `pc/tests/`，文件名以 `test_` 开头。

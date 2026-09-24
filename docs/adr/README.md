# 架构决策记录 (ADR)

本目录记录 Remapad 项目中具有长期影响的重要架构决策、技术选型背景及其取舍权衡。

## 为什么需要 ADR

架构决策记录（Architecture Decision Record, ADR）捕获决策做出时的具体背景、考虑的候选方案及正面与负面影响，为后续维护团队和 coding agent 提供权威的决策依据，防止陷入重复推翻已有架构权衡的循环。

## 文件与状态规范

- **文件命名**：采用 `NNNN-slug.md` 格式，编号从 `0001` 开始单调递增，四位数字对齐。
- **正文断行**：正文一句一行，只在句末（`。`、`！`、`？`）断行，不按列宽硬断；单句超过 120 列时把它改写得更短。
  传给 `create_adr.py` 的正文按同一规则断行，脚本原样写入、不做换行处理；列表项的续行缩进到与首行文字对齐。
  行宽遵循 [.editorconfig](../../.editorconfig) 的 `max_line_length = 120`，表格行与必须整行执行的命令不受此限。
- **状态流转**：
  - `proposed`：决策正在提议或评审中。
  - `active`：决策已被批准并作为当前生效的架构基准。
  - `superseded`：决策已被新的 ADR 完整取代（取代关系必须在新 ADR 的 `Supersedes`/`替代` 元数据和本索引中可追溯，不得回写旧正文）。
  - `retired`：决策所针对的功能或组件已被废弃淘汰。
- **决策边界**：一份 ADR 仅专注记录一项具体的长期决策，正文只写背景、决策、考虑的方案与决策级影响。
  实测数据、抓包记录、性能数字、实现步骤、踩坑过程与文件级索引属于功能文档（协议与设备数据进 `docs/controller-*.md`，
  结构与实现进 `docs/ARCHITECTURE.md` 与 `docs/ABSTRACTIONS.md`），不进 ADR。日常的缺陷修复、小范围样式微调或无重大架构影响的重构无需编写 ADR。
- **历史记录不可变**：ADR 正文一经创建即冻结——标题、日期、背景、决策、考虑的方案、影响和正文格式都不得修改，
  既不为适配当前实现改写，也不为修正措辞或重新排版而改动。求证得到的新事实写入对应功能文档，不回头改 ADR。
- **一次性整理已完成**：2026-09-21 做过一次全量分层整理，把实测、抓包、踩坑与实现步骤移出各 ADR 正文（决策段未改），
  原文留在 Git 历史；此后不再受理任何历史正文修改。
- **取代关系必须追加记录**：发现新事实或需要改变架构基准时，必须创建新的 ADR，不得改写旧 ADR 的决策内容。新 ADR 必须在 `Supersedes`/`替代` 元数据中链接被取代的编号，并明确是完整取代还是仅取代其中的范围；部分取代时，旧 ADR 仍适用的范围必须保留为 `active`，并在本文件索引中说明。
- **旧 ADR 唯一允许的生命周期修改**：旧 ADR 只允许变更 `Status`/`状态` 元数据，以反映 `superseded` 或 `retired` 等状态；不得修改其他字段或正文。取代关系以新 ADR 和本索引为准，不得通过改写旧正文补充解释。
- **索引是强制清单**：新增、取代、部分取代或废弃任何 ADR 后，必须同步更新下方“当前决策”表。表格必须从 `0001` 开始按编号升序列出 `docs/adr/` 下全部 `NNNN-*.md` 文件，不得遗漏、合并或只保留最后一条；状态必须与对应 ADR 元数据一致。

## 当前决策

下表是本目录的完整 ADR 索引，也是当前决策状态的唯一导航入口。
具体 ADR 只在需要查证某项功能的决策依据时，经下表索引定位后按需读取；
除此之外不允许直接读取或搜索 `docs/adr/` 下的 ADR 文件。

| ADR | 状态 | 主题 |
| --- | --- | --- |
| [0001](0001-use-pocketjs-vue-vapor-for-esp32s3-ui.md) | superseded | 采用 PocketJS 与 Vue Vapor 驱动 ESP32-S3 屏幕 UI；界面运行时选择由 0054 取代 |
| [0002](0002-adopt-hardware-bridge-and-packaging-architecture.md) | active | 引入统一硬件桥接协议与双工作区分层架构；打包部分由 0003 取代 |
| [0003](0003-use-official-esp-idf-host.md) | retired | 采用官方 PocketJS ESP-IDF host 构建链路；PocketJS 链路随 0054 从仓库整体移除 |
| [0004](0004-use-local-pocketjs-checkout.md) | retired | PocketJS 组件与原生归档改由本地 checkout 提供，Web 预览切换为官方开发主机；随 0054 整体移除 |
| [0005](0005-vendor-pocketjs-idf-components.md) | retired | ESP-IDF 组件与原生归档固定在本仓库，pocketjs 仅作开发参考；PocketJS 组件与原生归档随 0054 从仓库移除 |
| [0006](0006-product-owner-task-for-pocketjs-guest.md) | retired | 由产品 owner task 承载 PocketJS guest 生命周期；PocketJS guest 随 0054 整体移除 |
| [0007](0007-esp-lcd-panel-touch-bsp.md) | active | 显示与触摸 BSP 采用 esp_lcd 内置驱动与 Registry 触摸组件；strip 提交复用语义由 0008 部分取代 |
| [0008](0008-panel-transfer-completion-gate.md) | active | 面板提交增加传输完成门控，部分取代 0007 的颜色缓冲复用语义 |
| [0009](0009-ota-storage-flash-layout.md) | active | 固化 16MB Flash 分区终局布局：OTA 双分区与通用存储区 |
| [0010](0010-nimble-ble-controller-stack.md) | active | BLE 手柄外设采用 ESP-IDF 内置 NimBLE 栈，SMP 关闭、配对由应用层 Command 0x15 承担 |
| [0011](0011-controller-dataplane-module-boundary.md) | active | 控制器数据面按 ns2/ble/dp/usb 四模块分层，数据流单点汇合于 dp_task；目录划分由 0021 部分取代，数据面汇合约定仍生效 |
| [0012](0012-firmware-boot-splash-before-ui.md) | active | 固件在 PocketJS UI 就绪前自绘启动画面并提前点亮背光 |
| [0013](0013-defer-page-mount-after-first-frame.md) | superseded | 首帧只挂载首页，其余页面按帧补挂并在首页显示加载提示；由 0014 取代 |
| [0014](0014-page-mount-on-demand-progressive-fill.md) | superseded | 页面改为按需挂载并逐帧自顶向下填充，外层容器先出现；由 0015 取代 |
| [0015](0015-restore-deferred-page-mount-after-first-frame.md) | superseded | 恢复首帧后逐帧补挂页面，放弃按需挂载与分帧填充；由 0016 取代 |
| [0016](0016-mount-all-pages-before-first-frame.md) | active | 首屏前一次性挂载全部页面，放弃首帧后逐帧补挂 |
| [0017](0017-display-path-and-scroll-frame-budget.md) | active | 面板 SPI2 取 40 MHz、strip 改 32 行条带优先内部 RAM、本机加速回调接管填充与掩码混合；整幅内容按行带顺序在一帧内刷完（隔行因实机纵向错位被否决）；面板时钟取值由 0018 部分取代，60 Hz tick 的帧预算取值由 0037 部分取代；随 0054 换用 Slint 渲染器，条带取值与本机加速回调不再适用（行带缓冲放内部 RAM、一帧内刷完 damage 的做法沿用） |
| [0018](0018-panel-spi2-clock-80mhz.md) | active | 面板 SPI2 时钟取上限 80 MHz，部分取代 0017 决策 1 的时钟取值 |
| [0019](0019-playwright-e2e-and-host-unit-tests.md) | active | 以 Playwright 端到端测试与固件主机端单元测试作为回归基线；Playwright 那半由 0054 的界面宿主用例取代，固件主机端单元测试仍生效 |
| [0020](0020-battery-adc-sampling-and-charge-inference.md) | active | 电池电量走 BAT_ADC 采样（过采样平均 + 曲线拟合校准 + 静置电压—容量表），充电状态按电压趋势推断 |
| [0021](0021-input-path-three-stage-layering.md) | active | 输入通路按 input/pad/target 三段分层（接收 / 处理 / 转换），部分取代 0011 的目录划分 |
| [0022](0022-ota-over-bridge-frames-with-rollback.md) | active | OTA 升级复用桥接帧（USB-Serial/JTAG 双分区回写）与回滚健康门槛 |
| [0023](0023-ns2-sub-spec-conn-interval-and-wake-burst.md) | active | NS2 手柄链路采用亚规范连接间隔（5 ms）与显式唤醒广播（0x81 突发）；广播形态与唤醒突发由 0024 部分取代，连接间隔与特性启用门槛仍生效，上报节奏一项由 0034 确认并写死 |
| [0024](0024-ns2-steady-wake-adv-and-pairing-key.md) | active | 已配对身份常驻唤醒广播（0x81 + 主机地址）自动回连，配对页按钮改真机配对键语义，切换手柄走断连重连；部分取代 0023 的广播形态与唤醒突发；广播里的主机地址来源由 0030 部分取代，常态广播形态由 0031 部分取代，其中「上报节奏 15 ms 仍生效」经实机复测确认 |
| [0025](0025-pad-layout-modules-per-series.md) | active | 家族布局按系列分文件登记（layout.h/layout.c + layouts/），注册表统一匹配；补全 0021 在 pad/ 内的文件划分 |
| [0026](0026-same-generation-input-passthrough.md) | active | 手柄输入按同代透传、异代解析分发：设备自带报告语言与目标语言一致时原样转发报文体 |
| [0027](0027-runtime-usb-role-switch.md) | active | USB host 直插采用运行时角色切换（日志改走 UART0），复位回到串口；PWR 长按这一入口由 0038 部分取代 |
| [0028](0028-pad-combo-captures-screen.md) | active | 手柄组合键 L1+R1+L3+R3 捕获为屏幕操控（方向键移动焦点、圆圈键确认），捕获期间停发报文并补一帧全松开 |
| [0029](0029-pad-ui-axis-split.md) | superseded | 手柄操控屏幕的方向键分两个轴：上下走页面内容（到末尾再按下继续滚到页底）、左右只在底栏两项之间走；由 0041 取代 |
| [0030](0030-ns2-wake-adv-host-address.md) | active | NS2 唤醒与回连广播携带主机最近一次连接记录到的地址（凭证地址作兜底），部分取代 0024 的广播地址来源 |
| [0031](0031-ns2-reconnect-adv-and-explicit-wake-window.md) | active | NS2 常态广播回到 0x00 回连形态（不叫醒休眠主机），0x81 收成显式唤醒窗口，调试页 HOME 按实体手柄语义分流；部分取代 0024 的常态广播形态，其中「常态回连广播」一项由 0038 部分取代 |
| [0032](0032-ns2-fw-update-masquerade.md) | active | 主机推手柄固件更新时按「接住数据、逐帧空体应答、不重启」伪装（重启改由串口一次性武装），上报版本固化在 CONFIG_DEFAULT_FW_VERSION_* |
| [0033](0033-pc-single-process-tool-and-device-screenshot.md) | active | PC 侧工具合并为单进程会话（remapadctl：转发 + 命令行 + 截图 + OTA），桥接协议新增设备 → PC 的图像帧与串口 shot 命令把实机画面回传成 PNG |
| [0034](0034-ns2-report-interval-fixed-15ms.md) | active | NS2 上报节奏固定 15 ms、不提供运行时档位（5 ms 试验被否：四成以上通知因 mbuf 耗尽被丢）；确认 0023 的上报节奏一项 |
| [0035](0035-ns2-headset-state-passthrough.md) | active | NS2 耳机状态按输入设备的 3.5 mm 状态透传（0x09 的 0x0D 与 0x05 的插入位同源，串口 headset 可覆盖） |
| [0036](0036-host-pressure-triggered-js-gc.md) | retired | JS 堆的显式 GC 由宿主按 PSRAM 压力触发（每帧读余量、跌过 256 KiB 步长才 JS_RunGC 并重钉基线），与上游 PSP host 的 arena-pressure GC 同构；JS 堆随 0054 移除 |
| [0037](0037-ui-tick-rate-30hz.md) | superseded | UI 帧节奏定为 30 Hz：tickHz 写进 host profile，UI 侧以 ui/src/tick.ts 为唯一换算来源（TICK_HZ 取框架烘焙值），与帧绑定的时长写成毫秒再换算、序列图动画按累计帧推进以保持感官时间；部分取代 0017 的 60 Hz 帧预算取值，其中 30 Hz 这一取值由 0052 取代，tickHz 与 host profile 随 0054 作废 |
| [0038](0038-user-initiated-connection-window.md) | active | 连接由用户发起：上电与断连（主机睡下）静默，连接键（配对页「连接」、PWR 长按 3 秒）开 30 秒连接窗口广播、HOME 在未连接时开 10 秒唤醒窗口，窗口到期或主机连上即收窗；部分取代 0031 的常态回连广播与 0027 的 PWR 长按入口 |
| [0039](0039-pro-controller-only.md) | active | 设备对外只模拟一台 Pro Controller 2：身份、专用输入通道、报文体与会话回到单身份单连接，JoyCon 形态（左/右身份、0x07 / 0x08 报文体、导轨键确认与 side / rails 开关）整体移除；部分取代 0026 的透传身份约束 |
| [0040](0040-pc-gui-customtkinter-console.md) | active | PC 侧新增图形界面入口 remapadgui.py（CustomTkinter）：与命令行共用 Session 与串口，输出改走可注入的 Reporter，界面只做队列排空、命令投递与事件映射 |
| [0041](0041-clover-carousel-and-pad-axis-navigation.md) | active | 四叶草菜单左右滑动轮播与手柄双轴交互重构：内容单屏固定不溢出不滚动，左右无限轮播，方向键左右切页、上下选控件，底部三态栏按优先级展示且中区不参与手柄焦点；取代 0029 |
| [0042](0042-ds5-audio-haptics-onboard-synthesis.md) | active | DualSense 直插的触觉反馈走板上合成的音频通道：自写最小 UAC1 等时 OUT 客户端（48kHz/4ch PCM，后两路触觉、扬声器恒零），布局行 `out.audio_haptic` 声明能力，音频接手时 USB 路 HID 震动让位、桥接照旧 |
| [0043](0043-ds5-bridge-pc-side-audio-haptics.md) | active | DualSense 桥接路径的音频触觉由 PC 侧合成：remapadctl 对 4ch 端点开 WASAPI 流驱动通道 3/4，参数吃 FEEDBACK 帧（载荷扩到 16 字节带频率落地值），经 `haptic audio on\|off` 告知设备把桥接 HID 震动让位；延伸 0042 的合成刻度与让位语义 |
| [0044](0044-amiibo-bridge-upload-nfc-tag-emulation.md) | active | amiibo 镜像经桥接帧（0x40-0x43，逐帧 ACK）上传落 storage 分区 SPIFFS 固定 200 槽（572B 记录 = 镜像 + 厂商签名），选中持久化、重启恢复；NFC 命令通路（Command 0x01）由 target/ns2/ns2_nfc.c 软件模拟 NTAG215 标签，应答体按抓包布局钉在主机端用例 |
| [0045](0045-host-output-raw-capture.md) | active | 主机输出的原始字节（布局解析之前）经 dp_capture 环形队列与新桥接帧 0x12（HOST_RAW）回传 PC 落盘：默认关闭、串口 capture on\|off 开关，PC 侧 --capture / :capture 接住，与手柄转发同会话共存 |
| [0046](0046-ns-waveform-to-ds5-pcm-hd-haptics.md) | active | NS2 波形（每侧 3 个时序子帧）经布局行 `hd` 规则重整为 DS5 的 PCM HD 触觉：USB 4ch 承载频道 3/4 音圈 + 1/2 发声、蓝牙走 0x32/0x36 私有报告（3kHz 2ch s8 + CRC32；0x36 另带「一帧装整拍」的 Opus 喇叭块，需先显式路由手柄喇叭），映射与落地只在固件布局内、PC 哑渲染；振幅按 `hd` 的增益定标（DS5 两行 4 倍），私有流默认启用；延伸 0042/0043 的合成刻度与让位语义，FEEDBACK 扩到 57 字节 |
| [0047](0047-ds-behavior-settings.md) | active | DS4/DS5 手柄行为两项设置（触摸板映射加减键默认关、截图键默认开）持久化在 NVS，数据面每拍按先触发半区改写触摸板按下的键位；触摸点改为 4 字节解析、按左右半区建模，四个 PS 布局行登记触摸偏移与量程 |
| [0048](0048-pc-gui-settings-tab-mirrors-device-ui.md) | active | PC 图形界面新增「设置」页镜像设备 UI 的可改项（亮度与息屏、手柄配色、DS4/DS5 行为、电源，只读设备信息），读写走固件 CLI、控件值全取回读行（remapadctl.parse_device_reply）；调试动作不再放按钮，只在「命令」页填进输入框 |
| [0049](0049-firmware-draw-list-damage-diff.md) | superseded | 结构变化帧的 damage 由固件侧 draw list 差分算出：框架报整屏重画时换成本机差分出的真实变化区域，失败一律退回框架计划；随 0054 换用 Slint 渲染器（damage 由 Slint 自己算）不再适用 |
| [0050](0050-repaint-friendly-screen-rules.md) | active | 屏幕动效按重绘友好选：圆角加边框的元素必须带底色，只有拖动预览保留跟手平移；「切页瞬时完成、方向提示交给翻页箭头弹一下」已被翻页滑入 16px（90 ms，见 docs/ARCHITECTURE.md）取代 |
| [0051](0051-opaque-565-card-artwork.md) | superseded | 卡片底图烘成不透明 PSM_5650（SVG 垫根容器色底 + 位图清单声明 565），走渲染器直拷路径；PSM_5650 与直拷回调随 0054 换用 Slint 渲染器不再适用 |
| [0052](0052-ui-tick-rate-back-to-60hz.md) | active | UI 帧节奏恢复 60 Hz（tickHz 以 host profile 为准），CST816T 触摸改连续点模式；部分取代 0037 的 30 Hz 取值；tickHz 与 host profile 随 0054 作废，60 Hz 的取值改由平台层的动画推进节拍（16 ms 一档）承担 |
| [0053](0053-usb-serial-phy-handback-on-role-switch.md) | active | host 切回串口时固件显式把内部 PHY 指回 USB-Serial/JTAG（进 host 前先放掉句柄），COM 口不必重启即可回来；交还失败时界面在切回后询问是否立刻重启，复位仍是保底恢复路径 |
| [0054](0054-screen-ui-slint-rust.md) | active | 屏幕 UI 改用 Slint + Rust：.slint 在构建期编译成 Rust 静态库链进固件、运行期走软件渲染器，业务逻辑零 unsafe（只在 C ABI 边界留口并在注释里写明原因），字形按界面用到的字符自动子集，界面用例改成宿主侧 #[test]；取代 0001 |
| [0055](0055-core-ui-split-optional-ui-build.md) | active | core 与屏幕 UI 分离：固件核心持 ui_service.h 契约（状态快照装配、动作分发、生命周期），界面组件与编译口径收进 ui/ 工作区、按 REMAPAD_UI 开关可选编入（OFF 纯 C 可编译、屏幕熄灭、设置走串口 CLI），控制面命令队列改由独立服务任务泵；延伸 0054 的构建边界 |
| [0056](0056-ble-controller-off-power-save-cadence.md) | active | 完全静默（无连接、无广播窗口、不在配对流程）持续够久后关闭整个 BLE 栈（控制器断电，射频不再发热），连接键 / HOME / 配对新主机按起栈意图重新带起来；省电档（BLE 栈未运行）把数据面与界面节拍降到 12 fps 等效，屏幕不熄灭、不做空闲自动息屏 |
| [0057](0057-single-uv-project-at-repo-root.md) | active | Python 依赖统一到仓库根一个 uv 工程：pc/ 的 hidapi / customtkinter / av / sounddevice 并入根 pyproject.toml，删除 pc/pyproject.toml 与 pc/uv.lock，命令一律写成 uv run python pc/<工具>.py，remapadctl 的默认镜像路径改按脚本位置解析；0040 里「依赖写进 pc/pyproject.toml」一句随本次调整作废 |

## 创建 ADR 脚本用法

项目已内置 ADR 生成脚本 [scripts/create_adr.py](../../scripts/create_adr.py)，基于 Python 3.10+ 运行。不要手动编写或硬编码文件编号，请统一通过该脚本创建。

### PowerShell 调用示例

```powershell
python .\scripts\create_adr.py . "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
```


### Bash 调用示例

```bash
python3 ./scripts/create_adr.py . "决策标题" --slug "short-slug" --status active --context "决策促成的背景、约束与痛点" --decision "最终确认的技术选择" --option "候选方案 A" --option "候选方案 B" --consequence "正向影响与需要承受的代价"
```

### 参数规则

- `--adr-dir`：ADR 相对目录，默认为 `docs/adr`（可省略）。
- `--slug`：文件名短标识，中文标题必须显式传入 ASCII 格式的 slug。
- `--status`：初始状态，默认为 `proposed`；已落地的既定架构决策应传入 `--status active`。
- `--supersedes`：当新决策替代旧决策时，传入被替代 ADR 的四位编号（如 `--supersedes 0001`）。

### 三条硬性备注

- 不要修改历史 ADR：新事实写进功能文档或新建一条 ADR，已有的正文不再动。
- 正文每行尽量短于 120 字符：一句一行，超长的句子改写而不是硬断。
- 实测数据、抓包记录、实现步骤与踩坑过程不进 ADR。

## ADR 结构模板

```markdown
# NNNN — 决策标题

- Status: active
- Date: YYYY-MM-DD
- Supersedes: none

## Context

促成决策的业务背景、硬件环境约束与技术痛点。

## Decision

明确的技术路线选择与核心设计。

## Options considered

列出的各备选方案及其对比取舍。

## Consequences

- 正面影响与带来的优势。
- 付出的成本、引入的约束或后续重估触发条件。
```

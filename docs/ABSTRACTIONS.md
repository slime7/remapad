# Remapad 核心概念与领域抽象

本文档记录 Remapad 的两条数据路径：屏幕 UI 路径（Rust，core 经 ui_service 契约对接）与 USB→NS2→BLE 控制器路径（ESP-IDF 原生 C）。
界面的编译口径、平台层与 C ABI 由本仓库自己持有：界面源码在 `ui/src/`，Rust 工作区（宿主用例包、固件组件与编译口径）在 `ui/` 下。
NS2 协议内容见 [controller-switch2.md](controller-switch2.md)，PS 家族手柄数据见 [controller-ps.md](controller-ps.md)。

## 领域术语表

| 术语 | 含义 |
| :--- | :--- |
| **界面组件** | 界面框架里的界面单元：`App` 是根窗口，页面与控件都是被复用的组件（`component`）。 |
| **属性绑定** | 界面源码里的声明式表达式：属性依赖别的属性，依赖一变就重算；固件只写 `in-out property`。 |
| **界面回调** | 界面把用户动作交回宿主：本项目的动作统一是 `action(name, value)`，确认键走 `activate-focused()`。 |
| **字形烘焙** | 构建期把界面里出现过的字符按字号表（12 / 14 / 16 / 24）烘成位图，运行期不解析字体文件。 |
| **字符集锚点** | `ui/src/app.slint` 里一条不可见的 `Text`，把只在运行期拼出来的码点钉进烘焙集合（漏了就上屏成空洞）。 |
| **Damage region** | 一帧中需要重新光栅化的矩形（界面框架按失效元素算），平台再把它折成行带。 |
| **行带（band）** | 一次面板提交的单位：damage 矩形按 48 行切分出来的横向条带。 |
| **平台层 / 宿主层** | `ui/slint_ui` 的 `platform.rs`（渲染、行带提交、触摸采样）与 `host.rs`（状态写入、动作分发、手柄焦点）。 |
| **状态快照 / 动作回调** | core 与界面之间的两条通路（契约在固件核心的 `ui_service.h`）：`remapad_ui_state_t` 每 50 ms 进界面一次，动作按名字出界面。 |
| **C ABI 边界** | Rust 与 C 之间唯一的一层（`abi.rs`、`boundary.rs` 与 `include/slint_ui.h`）；Rust 侧业务零 unsafe，只有这一层留口并逐处注明原因。 |
| **UI 提供者** | `ui_service.h` 生命周期入口的实现方：带 UI 构建是 `ui/slint_ui` 组件（Rust），无 UI 构建是 `main/ui/ui_stub.c` 空实现；固件核心对界面框架零依赖。 |
| **硬件驱动层** | `firmware/main/drivers/`：面板、触摸、背光、按键、蜂鸣器与电池，屏幕平台只经 hooks 调它。 |
| **USB input** | 由 ESP32 USB host 接收的外部输入报告，先进入产品数据面，不直接进入界面。 |
| **接收段（input/、usb/）** | 输入通路的第一段：桥接帧的编解码与串口分帧、USB-Serial/JTAG 的唯一读取者、USB host 枚举与 HID 收发，以及实现 `dp_source_t` 的桥接源与 USB 源。 |
| **处理段（pad/）** | 输入通路的第二段：家族布局表把各家手柄报告解析成私有格式 `pad_state_t`（按键按位置语义、摇杆归一、能力位）。 |
| **转换段（target/）** | 输入通路的第三段：目标编码器 `pad_target_t` 把私有格式编码成具体目标家族的报文，现役实现为 `target/ns2/`。 |
| **桥接帧** | PC 与设备之间的分帧载荷：帧头（`A5 5A` + 版本/类型/槽位/序号/长度）+ 载荷 + CRC16，与 CLI 文本共用一根 USB-Serial/JTAG；帧类型表见下文「输入通路」。 |
| **同代透传** | 设备自带的报告语言与目标语言一致时，把设备报文体原样交给目标发送（NS2 手柄 → NS2 主机），只重写由本机会话决定的状态字节。 |
| **输出报告（反馈）** | 主机下发的震动 / 玩家灯 / 触觉采样经 `pad/feedback.c` 按设备布局行编码成该手柄的输出报告：USB host 直插写 OUT 端点，桥接路径把原始报告交给 PC 写回。布局与 HD 触觉的承载见 [controller-ps.md](controller-ps.md)。 |
| **主机输出原始采集（capture）** | 诊断通道：主机写进输出特征值的原始字节在解析与编码之前经桥接帧 `0x12` 回传 PC 落盘；默认关闭，串口 `capture on|off` 开关，PC 侧 `--capture` / `:capture` 接住。 |
| **OTA 会话（ota/）** | 升级通道的固件侧：`ota_session` 负责帧队列、flash 写入与重启，`ota_proto` 是纯逻辑的序号判定与窗口应答；镜像写进非运行分区、校验通过后切启动分区（链路见 [ARCHITECTURE.md](ARCHITECTURE.md) 的「OTA 升级通路」）。 |
| **NS2 report encoder** | 将私有手柄状态（`pad_state_t`）编码为目标 NS2 手柄的 USB/BLE 报告，位于 `firmware/main/target/ns2/`。 |
| **BLE controller peripheral** | 对 NS2 主机执行广播、GATT 服务、输入通知、输出命令和配对状态管理的 ESP32 外设角色。 |
| **Product control plane** | UI bridge 与固件控制面，用于低频状态、配置、配对操作和诊断；不承载高频输入报告。 |

## 界面契约：视口、字号与字体

界面的编译口径收在 `ui/build-support` 一处（宿主用例包与固件组件的 `build.rs` 共用）：

```mermaid
flowchart LR
    View["ui/src/app.slint 根窗口<br/>240 × 280"] --> Compile["构建期编译"]
    Sizes["字号表 12 / 14 / 16 / 24"] --> Compile
    Fonts["assets/fonts/<br/>NotoSansSC / MaterialIcons / seguisym"] --> Compile
    Compile --> Glyphs["字形位图 + 光栅化底图<br/>按界面用到的字符自动子集"]
    Glyphs --> Lib["libslint_ui.a"]
    Platform["platform.rs 的 VIEW_WIDTH / VIEW_HEIGHT"] --> Lib
```

- **视口**：根窗口与平台窗口都取 240 × 280；两处不一致时画面会被裁掉或留白。
- **字号**：只用 12 / 14 / 16 / 24 四档（`theme.slint` 的 `text-*` token 与 `ui/build-support` 的 `FONT_SIZES` 一致），
  新增档位只改这一处，否则只会退回最近的一档位图。
- **字体**：正文用 NotoSansSC（宿主与组件的 `build.rs` 把它设成 `SLINT_DEFAULT_FONT`），
  图标与转圈由 `components.slint` / `pages.slint` 里的 `import "../assets/fonts/*.ttf"` 引入；字体文件本身不进固件。
- **自动子集**：烘焙集合就是界面里出现过的字符；运行期才拼出来的文本（电量、内存、版本号）必须把用到的码点
  写进 `ui/src/app.slint` 的锚点串，否则上屏是空洞或豆腐块。
- **PC 预览**：`uv run python scripts/ui-preview.py` 打开 `ui/preview.slint`——设备画面（同一棵 `AppContent`，240 × 280）
  在上、控制条在下，动作在预览里按固件语义结算，因此点着就能走一遍界面；同一套字体与字号表，存盘即刷新。

## 控制器数据面

USB 到 NS2 BLE 的目标链路如下：

```mermaid
flowchart TB
    Bridge["PC 手柄（已实现）<br/>pc/ 桥接程序读原始报告并转发"]
    Host["USB host 手柄（已实现）<br/>手柄插在板卡上：OTG host 枚举 + HID 收发"]
    Recv["input/ 接收段<br/>帧解码 / 串口分帧 / dp_source_t 输入源"]
    Parse["pad/ 处理段<br/>家族布局表解析 + 归一 → pad_state_t"]
    Encode["target/ 转换段<br/>pad_target_t → NS2 报告编码（target/ns2/）"]
    Ble["BLE 广播 / GATT / 输入通知 / 输出命令"]
    Session["NS2 主机的连接与配对"]
    Feedback["pad_feedback_t：主机反馈（震动 / 玩家 LED / 触觉采样）"]
    FbEnc["pad/feedback.c<br/>按设备布局行编码输出报告"]

    Bridge -->|"桥接帧，USB-Serial/JTAG"| Recv
    Host -->|"IN 64B 中断传输（HID 报告 + Report ID）"| Recv
    Recv -->|pad_report_t| Parse
    Parse --> Encode
    Encode --> Ble
    Ble --> Session
    Ble -.-> Feedback
    Feedback --> FbEnc
    FbEnc -->|"OUT 中断传输（手柄插板卡）"| Host
    FbEnc -->|"OUT_REPORT 帧 → PC 写手柄"| Bridge
```

两条输入路径都已落地（PC 桥接走 USB-Serial/JTAG 的桥接帧，USB host 直插走 OTG host 的 HID 中断传输）。
在 `input/` 汇合之后共用 `pad/` 与 `target/` 两段，解析与映射只有一份；反馈方向同样收敛在 `pad/feedback.c` 一处（按设备布局行编码输出报告，USB 写 OUT 端点，桥接把原始报告交给 PC）。

这条链路需要保持低延迟和确定性：

- USB 接收、解析、状态快照与 BLE 发送都用 ESP-IDF 原生驱动、任务和队列；NS2 报告编码按
  [controller-switch2.md](controller-switch2.md) 的型号、Report ID、摇杆打包与字节序实现。
- BLE manager 负责厂商广播字段、GATT service/characteristic、通知订阅、回连、唤醒与配对状态机。
  配对凭证（每条 6B 主机 MAC + 16B LTK，百字节级小 blob、仅配对成功时写一次）与 PHY 校准同住 NVS，
  按身份分槽、每身份保留最近 2 条，按 MAC 覆盖、配新主机是追加而非覆盖，因此不常驻「解除配对」入口；
  存储模型见 [controller-switch2.md](controller-switch2.md) 的「蓝牙配对信息区结构」。
- 广播状态位（厂商数据偏移 0x0B）是主机唯一的唤醒判据：`ns2_adv_payload()` 按信号组装参数成型三种形态，
  发现广播不带主机地址、状态 0x00，回连与唤醒广播携带最近一次会话记录到的对端地址（无记录时回退到最近一条非全零凭证）、状态 0x00 与 0x81；
  窗口经 `ns2_adv_window_open` 按「窗口时长 + 前置唤醒突发」打开，形态决策是
  `ns2_adv_choose_mode(paired, pairing_requested, window, now)` 纯函数，窗口到期未连接就彻底停发广播并关闭 BLE，
  等用户再按连接键。
- 连接间隔由主机下发（常为 4 单位即 5 ms），固件只观测不主动请求：ESP32-S3 由
  `CONFIG_BT_CTRL_BLE_MIN_CONN_INTERVAL_ENABLE` 放行亚规范间隔，NimBLE 主机侧按规范拒绝 itvl < 6 的请求。
- 输入被主机采纳的门槛是 **0x0C/0x04（启用特性）**：未启用的链路即使 itvl=4 也不采纳输入，输入通知只在启用后发送，
  已订阅却迟迟不启用的会话由休眠看门狗断开重连（`ns2_adv_dormant_link()` 判定）。
- `0x0E` 运动数据长度必须非零，按 40 字节零值占位。
- 耳机状态（3.5 mm）由输入设备派生：`pad_state_t` 的 `headset_present` / `headset_mic` 经 NS2 目标的单一来源映射成
  `0x09` 偏移 `0x0D` 与 `0x05` 的耳机插入位，编码路径与同代透传路径共用；主机接受的档位与输入设备的字节偏移见
  [controller-switch2.md](controller-switch2.md) 与 [controller-ps.md](controller-ps.md)。

- flash 写入期间 cache 被禁用：任务在禁缓存窗口里不能访问 PSRAM。凭证等持久化写因此收敛到 `ble_creds` 的内部 RAM 栈任务，
  各任务只更新内存表并投递快照；新增持久化需求沿用同一模式。面板提交、DMA 缓冲与界面行带同理都放内部 RAM。
- `firmware/main/bridge/` 是控制面：只承载低频的模式切换、配对开关、连接状态、电池与诊断。
  界面侧的动作经 `action` 回调交给 `main/ui/ui_service.c` 的统一分发，转成 JSON 命令排进队列；队列由
  `js_bridge_service_start()` 建起的控制面服务任务每 50 ms 服务一次（`js_bridge_service()`），
  入队出队经队列交接（无锁），PWR 按键与串口 CLI 等别的上下文经 `js_bridge_submit_command` 的外部队列转移。
  命令清单见 [pc/README.md](../pc/README.md) 与串口 CLI 的 `:help`。
- **屏幕文案一律取自界面源码的字面量**，固件只回状态码、不回可上屏的文本：
  字形只按界面里出现过的字符烘焙，固件回传的新字符串直接显示就是空洞或豆腐块（需要显示时把码点写进 `app.slint` 的锚点串）。
- 玩家序号灯（主机 Command 0x09 下发的 4 位掩码）由 `ns2_session_player_leds()` 按活跃会话汇总，
  经状态快照的 `player_led` 供底栏四格指示灯使用。
  用户设置（背光亮度、手柄四段配色、上报固件版本）由 `firmware/main/config/app_config.c` 持久化到 NVS（内部 RAM 栈提交任务，与 ble_creds 同一模式），开机恢复。
- USB 角色（`usb_role`：device = 插电脑 COM 口，host = 插手柄）在运行时真实切换，顺序与约束见
  [ARCHITECTURE.md](ARCHITECTURE.md) 的「USB 角色切换」；角色只在本次运行有效、不写 NVS。
- 底栏左区是 USB 模式指示：串口档电脑图标、手柄档手柄图标，图标与「USB 模式」页两张卡一一对应；
  对接对象没接上时换成同族的禁用字形、颜色降一档、标签写「未连接」——直插手柄接没接取 `usb_input_attached()` 与 `pad_family_from_ids()`，
  PC 接没接取 USB-Serial/JTAG 的 SOF 监视，两者都在状态快照里。
- 配对与连接状态接的是真实 BLE 会话（NimBLE 手柄外设）：配对页主按钮是连接键，广播中它发 `disconnect`（收窗口与流程、断开链路、静默）；
  副按钮配新主机走 `startPairing`（先断开当前主机再进发现广播等新主机搜索，凭证拿齐且会话注册完成才退出流程）；
  解除配对走显式 `unpair`（清 NVS 凭证并静默），UI 不暴露入口。
  已连接却停在握手等待态的主机（手机/PC 自动回连）由 3 秒无协议活动的空闲超时断开，主机连接地址是随机地址，不能按 OUI 识别。
  配对成功以协议证据判定（初始化握手完成、凭证匹配回连，或主机在链路上启用特性 0x0C/0x04），NVS 凭证只是重启后仍成立的持久化证据，两者独立；
  第三条证据覆盖主机换随机地址与主机已有凭证而不再重跑 0x15 两个现场，判据与用例见 `ns2_adv_host_registered()`；配对六态由此实时推导并写进状态快照。
  协议与凭证的展开见 [controller-switch2.md](controller-switch2.md)。
- 电池由 `battery.c` 真实采样（BAT_ADC=GPIO1 / ADC1_CH0，分压 3:1 还原 VBAT，静置电压—容量表折算百分比）；
  充电状态没有可测量的引脚，按电压趋势推断。
- **设备对外只模拟一台 Pro Controller 2**：
  身份枚举 `ns2_identity_t` 只有 `NS2_ID_PRO`，单身份、单连接、单广播实例，
  报告格式固定 `0x09`（USB 模式 `0x05`），专用输入通道只注册 Pro 那一条（主机按自家型号查特征值 UUID 才决定订阅），会话往主机订阅的句柄发通知。
  主机只接受 public 地址的广播（本机派生的静态随机地址只留作串口 `advaddr` 对账），一台控制器只有一个 public 地址，左右分槽的 JoyCon 形态因此被移除；
  协议事实留在 [controller-switch2.md](controller-switch2.md) 的 GATT 属性表与广播过滤各节备查。
  配对凭证按 `ns2_identity_t` 分槽持久化（`ble_creds`，NVS v2 格式，旧单表记录迁移进 Pro 槽），分槽结构保留以备将来再加型号。
  序列号 / PID / 出厂块（0x13000）按连接身份提供；配色按「机身 / 按键 / 高光 / 握把」四段（`0xRRGGBB`）配置，对应出厂块 `0x13019` 起的布局，
  手柄设置页给四款预设、串口 `ctrl` 手工指定；改色等价于旧手柄断电、新手柄上电：断开现有连接、重算出厂块并直接打开连接窗口，
  主机照回连形态自己连回来读到新颜色（未配对时没有主机可回连，保持静默）。
  会话层面向连接分槽（最多 2 个），桥接命令 / 应答 / 通知都带连接上下文，并维护每槽的报告计数与链路快照（`ns2_session_status()`，串口 `link` 与日志取用）；
  `controllerConfig` 应答带 `addresses.pro`（显示序大写十六进制，host 未同步时为空串），手柄设置页的身份信息行取用它。
- 配对对外的心智模型是「设备不主动发信号，连接要按连接键」：开机与断开后都静默，按连接键才广播（已配对身份发回连形态等主机连回来、未配对身份进配对流程）；
  主机睡下时按 HOME 把它叫醒并自动回连。主机侧配对记录在首次连接握手时完成，用户不需要在屏幕上做任何确认动作；屏幕配对页用于观察状态、按连接键、配新主机或断开。
- 主机推送的手柄固件更新按「接住数据、逐帧应答、不重启」处理：`0x0018` 上的记录流按帧装配
  （`main/target/ns2/ns2_upgrade.c`，字节级用例在 `firmware/test/test_ns2_upgrade.c`），
  帧凑齐即按指令通道格式回一条空体应答，主机因此把整包推完；收尾的 `0x0d/0x07` 之后默认不重启——重启会被主机当成更新没生效而重推整包，
  串口 `fwapply on` 才一次性武装收尾重启。上报给主机的固件版本固化在 `main/config/app_config.h` 的 CONFIG_DEFAULT_FW_VERSION_*，
  串口 `fwver a.b.c` 可临时覆盖并就地重建出厂块。
  协议见 [controller-switch2.md](controller-switch2.md) 的「Command 0x0D - 手柄固件更新推送」。
- 调试注入是控制面进入数据面的唯一低频通道，采样与编码仍由数据面任务独立完成（`firmware/main/dp/dp_source.c`）：
  按键注入经 `dp_source_inject()` 叠加一次按下并按时长自动释放，摇杆注入经 `dp_source_inject_stick()` 给出持续电平（0-4095，两侧独立，未设定的一侧沿用输入源）；
  注入是合成的最后一步：按键叠加在合成按键上，设定过的摇杆覆盖合成摇杆。按键名表由 `dp_source_key_lookup()` 提供，串口 CLI 与主机端用例共用。
  HOME 按实体手柄语义分流（`ns2_adv_home_action()`）：主机在线时就是主页键，不在线时改成唤醒请求打开唤醒窗口——设备平时静默，这是唯一的叫醒路径；
  这条语义长在数据面上，因此实体手柄按 HOME 与调试页注入 HOME 是同一个动作，按钮文案跟着主机状态走。
  串口 `link` 按身份打印链路快照（`ns2_session_status()`）：对外广播地址、连接句柄、连接间隔（`itvl`，4 = 5 ms）、会话状态、报告格式、
  已开启的通知通道、特性启用位（`feat`）、已发送报告数、凭证条数与广播形态（`adv`）。
- 输入与输出已解耦成三段稳定接口：
  `dp/dp_source.h`、`pad/pad_state.h` 与 `target/target.h`。
  新增输入设备（桥接 PC、USB 手柄、调试注入）只需实现 `dp_source_t` 并注册：首个注册源拥有摇杆/扳机/触摸/运动/耳机状态/透传原始报文与设备标识字段，
  后续源叠加按键，调试注入最后叠加；合成只拷这些主源字段（`copy_primary_fields`），新增字段要一并加进去。
  目标侧 `target_send_pad()` 按注册的 `pad_target_t` 编码（现役 `target/ns2/`）。
- 反馈方向的完整链路：主机事件 → `pad_feedback_t` 持续帧 → `pad/feedback.c` 按设备布局行编码成输出报告
  （DS4 / DualSense / Xbox 蓝牙 / XInput / DS3 / NS1 各一行，NS2 手柄原样吃主机的 LRA 参数包）。
  震动流是音频式连续包络（低频给冲击、高频给纹理），每颗马达按布局行的 `rumble_band` 跟带、振幅按 `rumble_max` 缩放；
  「在震」判据是 LRA 状态字使能位且归一强度高过载波电平，零幅度保活包与查找手柄页的载波包都不算在震。
  主机振幅是 NS2 LRA 的线性档位，送给 ERM 马达时经 `pad_rumble_perceived` 感知重映射（音色表、CLI 注入与 HD 的 PCM 波形不过表——音圈没有 ERM 死区）。
  NS1 的行不走单字节强度：`out.rumble_style` 声明成波形编码后，每侧 4 字节直接吃主机的 LRA 波形
  （两带的频率与振幅各自编码，协议没给频率时按该带缺省频率补足，见 [controller-ns1.md](controller-ns1.md)）。
  采样是主机点播的声音（主机只发采样 ID，节奏与音色由 `pad/feedback.c` 的采样音色表给出）：输入设备有线接入时驱动板载蜂鸣器，
  声明 HD 触觉的设备按布局行 `hd` 规则把 NS 波形重整成振荡器声部，其余设备丢弃。
  写回按编码后的报告字节变化才发，同代透传的参数包原样在编码字节里；
  映射与承载见 [controller-ps.md](controller-ps.md)。
  DualSense 直插时反馈优先走音频触觉通道（板上合成或 PC 侧合成），音频接手期间 HID 报告的震动字段清零、玩家灯照常；
  玩家灯只落四颗白灯、灯条不驱动（写条会连帧重写玩家色并带淡出设置）。
  上报主机的电量跟输入设备走：家族表置 `PAD_CAP_BATTERY` 且本帧解出电量的设备经 `target_apply_pad_battery` 覆盖事实表后随报告上发，
  板载电池只在设备没报电量时兜底，0x05 报文专用的端电压字段按电压—容量表反演成名义值。
- amiibo 由软件模拟一张 NTAG215 标签：镜像经桥接帧上传落 storage 分区 SPIFFS 槽位（`amiibo/`，572 字节记录 = 镜像 + 厂商签名，200 槽，选中持久化、重启恢复），
  主机的 NFC 命令（Command 0x01）由 `target/ns2/ns2_nfc.c` 按协议布局应答；
  布局与命令细节见 [controller-switch2.md](controller-switch2.md) 的 NFC 章节。
- USB host 直插的数据面：`usb/usb_transport.c` 装 host 栈、枚举并按报告描述符挑手柄用途的 HID 接口，
  `usb/usb_input.c` 把 IN 报告组成 `pad_report_t` 交给同一份家族表并把反馈写回 OUT 端点；
  声明音频触觉能力的设备（DualSense）另由 `usb/usb_audio.c` 认领 UAC1 音频流 OUT 接口，持续向等时端点送板上合成的 4ch PCM（频道 3/4 音圈、1/2 小喇叭）；
  纯逻辑的描述符解析与 PCM 合成在 `usb_audio_parse.c` / `haptic_synth.c`（主机端可测）。
- 运动数据：布局行描述取样位置、样本数、样本内字段顺序（NS1 的 6 轴样本是加速在前）、轴映射（NS1 一次三份取最新一份）
  与设备原始刻度，解析进按统一刻度表示的 `pad_motion_t`（加速 4096 计数每 g、陀螺 14247 计数每 1000 °/s；
  行里的 `accel_per_g` / `gyro_per_dps_x1000` 声明原始刻度，NS 家族与统一刻度相同，因此不声明换算）；
  轴向取 NS 家族的约定，PS 家族的轴向与符号待实机核对（见 [controller-ps.md](controller-ps.md) 的核对状态）。
- 0x05 报文的 IMU 字段按 [controller-switch2.md](controller-switch2.md) 的偏移填真值；0x09 的 40 字节运动块结构未公开，
  因此只提供 CLI `motion 3` 的实验填充档。
- USB 高频输入不应经过控制面队列，也不应等待屏幕刷新或状态轮询。

数据面每拍的节奏收口在这一处：

```mermaid
flowchart LR
    Sample["dp_task 每 5 ms 采样一次输入源"] --> Send{"到第 3 拍（15 ms）？"}
    Send -->|"是"| Report["target_send_pad → BLE 输入通知（DP_SEND_DIV=3，无运行时档位）"]
    Send -->|"否"| Sample
    Host["主机反馈事件（震动 / 玩家灯 / 触觉采样）"] --> Merge["叠加进 pad_feedback_t 持续帧"]
    Tone["采样音色的段状态（幅度段 + 段音高，按 5 ms tick 变化）"] --> Deliver
    Merge --> Deliver{"写回语义变化？"}
    Deliver -->|"是"| Out["按布局行编码 → OUT 端点 / 桥接 0x11 帧"]
    Deliver -->|"否"| Merge
```

投递条件：主机事件带哪些字段就覆盖哪些字段（震动与玩家灯是持续状态，回落到默认值会把刚点亮的玩家灯写灭）；
触觉采样只在带它的事件里更新、0x00 是停止，段边界不能只跟主机事件走——那会把采样音色的段量化到 64ms 的栅格，数据面再以 300ms 超时自灭兜底。

## 输入通路：接收 / 处理 / 转换

输入通路按三段划分：
`input/` 只把字节变成「原始报告 + 设备标识」，`pad/` 只把原始报告变成私有格式并收敛家族差异，`target/` 只把私有格式编码成目标报文。三段之间是单向数据流：
新增一种手柄只在 `pad/layouts/` 里加一行（新系列则加一个文件并登记），新增一个目标（例如将来的 NS1）只加一个 `pad_target_t` 实现。

```mermaid
flowchart LR
    subgraph PC["PC（pc/ 桥接程序）"]
        HID["手柄 HID 报告"] --> BR["remapadctl.py：原始报告 + 设备标识"]
    end

    BR -- "桥接帧（USB-Serial/JTAG）" --> LINK

    subgraph FW["ESP32-S3 固件 firmware/main/"]
        LINK["input/ 接收段<br/>input_link 唯一读取者 + input_frame 解帧"]
        SRC["input/ 输入源<br/>dp_source_t 实现"]
        CLI["console/ CLI 行解析"]

        LINK -- "非帧字节" --> CLI
        LINK -- "桥接帧" --> SRC
        SRC -- "pad_report_t" --> DEV["pad/ 处理段<br/>家族表 + pad_state_from_report"]
        DEV -- "pad_state_t" --> TGT["target/ 转换段<br/>pad_target_t"]
        TGT --> NS2["target/ns2/<br/>0x05 / 0x09 报告编码"]
        NS2 --> BLE["ble/ NimBLE 输入通知"]
        BLE -. "主机反馈 → pad_feedback_t" .-> LINK
    end

    BLE --> HOST["NS2 主机"]
```

私有格式是这条通路的接缝：上游只要能填出 `pad_state_t`（桥接 PC、将来的 USB host 直插、调试注入都一样），下游目标就不需要知道手柄从哪来。

```mermaid
classDiagram
    class pad_report_t {
        pad_family_t family
        pad_conn_t conn
        uint16 vid
        uint16 pid
        uint8 report_id
        uint8 len
        uint8 data 64 字节
    }

    class pad_state_t {
        uint32 buttons
        uint16 axis 四轴
        uint16 trigger 双扳机
        pad_touch_t touch 两处
        pad_motion_t motion
        uint16 mic_level
        bool mic_muted
        bool headset_present
        bool headset_mic
        uint8 battery_percent
        bool charging
        uint32 caps
        pad_family_t family
        pad_conn_t conn
        uint16 vid
        uint16 pid
        uint8 report_id
        uint32 seq
    }

    class pad_target_t {
        const char * name
        uint32 caps
        set_facts()
        send_pad()
    }

    class pad_feedback_t {
        bool rumble_on 双马达
        uint8 rumble_strength 双马达
        uint8 rumble_raw 目标原始参数
        pad_rumble_key_t rumble_keys 每侧 3 个时序子帧
        uint8 player_led
        uint8 haptic_sample
        uint8 haptic_env 音色当前段
    }

    pad_report_t --> pad_state_t : pad_state_from_report
    pad_state_t --> pad_target_t : target_send_pad
    pad_feedback_t ..> pad_state_t : 反向链路（目标 → 输入设备）
```

- 按键位按位置固定、键名沿用 PS（`PAD_BTN_TRIANGLE` 上、`PAD_BTN_CIRCLE` 右、`PAD_BTN_CROSS` 下、`PAD_BTN_SQUARE` 左）：
  Xbox 与 Nintendo 的 A/B/X/Y 标签位置不同，用 PS 名可以避免「A 到底指哪个键」的混淆，家族表把各家的物理键填进对应位置；背键与静音键（目标侧作 C 键）用扩展位占位。
- 四轴与双扳机统一为 0-4095 整数、摇杆中位 2048，Y 轴统一成「上为正」，8% 死区在解析段套用并把剩余行程重新铺满；扳机保持模拟量，是否数字化由目标决定。
- `caps` 标注这一帧里哪些字段真的来自设备（运动、触摸板、模拟扳机、背键、麦克风、电池、震动）；型号未识别时按 XInput 形态兜底并置 `PAD_CAP_FALLBACK_LAYOUT`，结果仍可用但字段可能错位。
- 触摸板按左右半区建模：一帧最多两个触点（DS4 与 DualSense 都是每点 4 字节——触点字节 bit7 为 0 表示有触点，其余三字节是 12 位 X 与 12 位 Y），
  按归一后的 X 分到 `touch[PAD_TOUCH_LEFT]` / `touch[PAD_TOUCH_RIGHT]`，同一半区保留先出现的那一路，归一值夹进 0-4095。
- 目标只消费自己 `caps` 范围内的字段：不在集合里的部分（IMU、触摸板、麦克风）不映射，能力集合变化时提示一次，不逐帧刷日志。
- 桥接帧与 CLI 文本共用一根 USB-Serial/JTAG：接收侧校验 CRC、失步时只丢一个字节继续扫描，非帧字节原样交回命令行解析，因此桥接跑着的时候串口 CLI 照常可用。
- 布局行现在分三组描述：输入字段（既有）、运动字段（`motion`）与输出（反馈）报告（`out`），外加设备自带的报告语言与期望身份（`native_lang` / `native_identity`）；
  输入字段里除偏移与按键位图，还声明报告长度范围（同一 Report ID 下按报文长度分行的形态，如精英手柄 2 的三份报文）、
  背键字节与位映射（含「背键已交给手柄内部配置档」的判定字节）、数字扳机位（NS 的 ZL / ZR 只有位）、
  帽子的编号方式（PS 系 0 起算、Xbox 蓝牙 1 起算）与摇杆 / 扳机的宽度形态（单字节、16 位有符号、16 位无符号、12 位打包、10 位扳机）。
  `out` 里的 `frame` 标出报告的收尾方式：PS 系的蓝牙形态要在末 4 字节补 CRC32（种子字节 0xA2 参与计算，见 `pad/feedback.c`），缺它的报告手柄整份都不接受（实机表现：写回成功、毫无反应）；
  `led_mask_map` 把主机玩家灯掩码落到设备自己的灯位模式（DualSense 的五颗灯是固定模式，1P 只有中灯、2P 中灯加外灯，不能直写主机掩码）；
  `audio_haptic` 声明设备带可驱动的音频触觉通道（DualSense 两种连接方式都有）：USB 直插时是 UAC 通道、由 `usb/usb_audio.c` 接管震动渲染，
  蓝牙接入时是 0x32/0x36 私有触觉流、由 PC 侧接管（默认启用）；
  `presets` 还承担手柄喇叭的路由：音频控制的输出路径位段要显式置成手柄喇叭（0x30）、前级 +6dB，
  并带上对应的更新使能位与音量档——不路由时内置喇叭处在未路由状态，发声段送进去全被丢掉（实机：0x36 触觉可达而喇叭无声）；
  `hd` 是 HD 触觉波形映射规则（承载采样率/峰值、两带频率落地范围与缺省、采样强震与发声频率、
 振幅增益 `gain_num/gain_den`——主机游戏内档位很小，DS5 两行按实机 A/B 取 4 倍，增益在写 FEEDBACK 帧之前落地；
  `ops = 0` 表示无 HD 通路）——
   NS 的震动是波形描述，映射在布局内完成。
  `rumble_style` 选震动编码方式（单字节强度或 NS1 的每侧 4 字节波形），`no_report_id` 声明报告不带
  Report ID（XInput 形态的报文首字节就是自己的类型字节）。
  DualSense 的灯条不参与反馈：玩家号只上四颗白灯，灯条颜色留给 PC 侧管理——写条会连帧重写玩家色并带「淡出」设置，一震就变色、平时淡回默认白（实机撤出）；
  若真要在蓝牙上写灯条，设置与颜色必须同一帧（主机的连接动画会一直盖着灯），这条实机结论留档备用。
- 未登记的 VID/PID 仍回落 XInput 形态并置 `PAD_CAP_FALLBACK_LAYOUT`；
  厂商 VID 分不开布局的第三方手柄（XInput 形态，各家 VID 不同）由布局模块的型号表定家族，
  型号表只列公开实现里登记为这份报文的型号，不按 VID 一把抓——同一厂商的另一种模式往往是另一个 PID、另一份报文。
  Xbox 家族（VID `0x045E`）与 XInput 形态各占一个文件，Xbox 按蓝牙报告的三份报文长度与精英手柄 2 的 PID 分行；
  Nintendo 家族（VID `0x057E`）按系列文件 `pad/layouts/ns.c` 登记，NS2 的 0x05 / 0x09 报文体与 NS1 的
  两代报文（0x30 标准报文 / 0x3F 简单报文，按键与摇杆偏移互不相同）分别登记。

帧类型（固件侧定义在 `firmware/main/input/input_frame.h`，PC 端在 `pc/link.py` 镜像一份）：

| 类型 | 方向 | 载荷 |
| :--- | :--- | :--- |
| `0x01` ATTACH / `0x02` DETACH | PC → 设备 | 8 字节设备标识（家族 / 连接方式 / VID:PID / Report ID / 报告长度） |
| `0x10` REPORT | PC → 设备 | 设备标识 + 原始报告（最多 64 字节） |
| `0x11` OUT_REPORT | 设备 → PC | 要写回手柄的输出报告原始字节（首字节是 Report ID，最多 78 字节） |
| `0x12` HOST_RAW | 设备 → PC | 主机输出原始采集：通道字节（GATT 句柄低字节）+ 标志/长度（bit7 截断、低 7 位数据长度）+ 原始字节（最多 253）；帧头 slot 是设备侧记录号，跳号即队列满丢包。默认关闭，串口 `capture on` 打开 |
| `0x20` FEEDBACK | 设备 → PC | 左右震动使能与两带强度、玩家灯、触觉采样（原始采样 ID，仅日志展示）；16 字节版再带两带驱动频率落地值（u16 小端 ×4）；57 字节 HD 版再带固件按布局行重整出的时序子帧表（每侧有效子帧数 + 3×（低频频率 u16 LE + 低频增益 + 高频频率 u16 LE + 高频增益）+ 扬声器频率 u16 LE + 增益），PC 侧音频触觉与蓝牙私有流按它哑渲染；投递时机除主机事件外还看采样音色的段状态（幅度段 + 段音高）按 5ms tick 的变化——段由固件合成、主机只给采样 ID 与起停（查找手柄页约 15Hz），只跟主机事件投递会把段边界量化到 64ms 的栅格（震动/蜂鸣起止错位、短段丢失） |
| `0x30` OTA_BEGIN | PC → 设备 | `ROM1` + 镜像字节数（u32 小端） |
| `0x31` OTA_DATA | PC → 设备 | 块序号（u16 小端）+ 最多 200 字节镜像数据；帧内 `slot=1` 标记该窗口的末帧 |
| `0x32` OTA_END | PC → 设备 | 空 |
| `0x33` OTA_ACK | 设备 → PC | 状态 + 错误码 + 期望序号（u16 小端）+ 已收字节（u32 小端）；对 BEGIN 的应答末尾再附 16 字节运行版本 |
| `0x40` AMIIBO_BEGIN | PC → 设备 | 名称长度（u8）+ 名称（UTF-8，1-31 字节）+ 镜像字节数（u32 小端，必须是 540 或 572） |
| `0x41` AMIIBO_DATA | PC → 设备 | 偏移（u16 小端）+ 最多 200 字节镜像数据；偏移越过已收字节数报错，重复帧幂等 |
| `0x42` AMIIBO_END | PC → 设备 | 空 |
| `0x43` AMIIBO_ACK | 设备 → PC | 状态 + 错误码 + 已收字节（u32 小端）+ 槽位号（仅 DONE 有意义，0xFF 表示无） |
| `0x7F` PING | 双向 | 协议版本号（1 字节） |

解码器按线格式上限 255 字节收帧，报文帧仍按 72 字节语义校验（8 字节设备标识 + 最多 64 字节报告），
输出报告帧按 78 字节校验（DualSense / DualShock 4 的蓝牙输出报告长度）。
OTA 帧由 `input_link` 交给 `ota/ota_session`，amiibo 上传帧交给 `amiibo/amiibo_session`（逐帧回 ACK，
收齐后经 `amiibo_store` 落 storage 分区 SPIFFS 槽位），PING 由 `input_link` 直接应答，其余交给 `input_source`；
升级协议、流控与回滚门槛见 [ARCHITECTURE.md](ARCHITECTURE.md) 的「OTA 升级通路」。

PC 手柄到 NS2 主机的完整时序（映射表把家族差异收敛在 `pad/`，所以桥接路径与 USB host 直插路径共用后面两段）：

```mermaid
sequenceDiagram
    autonumber
    participant PC as pc/remapadctl.py
    participant RECV as input/input_link
    participant SRC as input/input_source
    participant DP as dp/dp_task
    participant DEV as pad/pad_device
    participant TGT as target/ns2
    participant HOST as NS2 主机

    PC->>RECV: ATTACH 帧（家族 / 连接方式 / VID:PID）
    RECV->>SRC: input_source_handle_frame
    PC->>RECV: REPORT 帧（设备标识 + 原始报告）
    RECV->>SRC: 存入最近一帧报告
    loop 每 5 ms（第 3 拍发一份报告）
        DP->>SRC: dp_source_sample()
        SRC->>DEV: pad_state_from_report()
        DEV-->>SRC: pad_state_t
        DP->>TGT: target_send_pad()
        TGT->>HOST: ns2_output_send() → BLE 输入通知
    end
    HOST->>DP: 主机反馈（震动 / 玩家 LED / 触觉采样）
    DP->>PC: FEEDBACK 桥接帧（写回语义变化才发；DS5 桥接时驱动 PC 侧音频触觉合成）
    PC->>RECV: DETACH 帧（拔线或退出）
    RECV->>SRC: 状态回静置，按键不卡住
```

家族与目标的按键对应关系（按位置对齐，因此 Xbox 的物理 A 与 PS 的 Cross 都落在 `PAD_BTN_CROSS`、再到 `NS2_BTN_B`）：

| 私有格式（位置语义） | Xbox 物理键 | PS 物理键 | NS2 目标 |
| :--- | :--- | :--- | :--- |
| `PAD_BTN_CIRCLE`（○ 右） | B | Circle | `NS2_BTN_A` |
| `PAD_BTN_CROSS`（✕ 下） | A | Cross | `NS2_BTN_B` |
| `PAD_BTN_TRIANGLE`（△ 上） | Y | Triangle | `NS2_BTN_X` |
| `PAD_BTN_SQUARE`（□ 左） | X | Square | `NS2_BTN_Y` |
| `PAD_BTN_L1` / `PAD_BTN_R1` | LB / RB | L1 / R1 | `NS2_BTN_L` / `NS2_BTN_R` |
| `PAD_BTN_L3` / `PAD_BTN_R3` | 左右摇杆按下 | L3 / R3 | `NS2_BTN_LSTICK` / `NS2_BTN_RSTICK` |
| `PAD_BTN_TOUCHPAD`（左侧小键） | View（select） | SHARE / Create | `NS2_BTN_MINUS`（减号） |
| `PAD_BTN_OPT`（选项） | Menu | Options | `NS2_BTN_PLUS`（加号） |
| `PAD_BTN_HOME`（主页） | 西瓜键 | PS 键 | `NS2_BTN_HOME` |
| `PAD_BTN_SHARE`（分享类） | 分享键（Series 手柄） | 触摸板按下 | `NS2_BTN_CAPTURE`（截图） |
| `PAD_BTN_MUTE`（静音） | 无 | DualSense 静音键 | `NS2_BTN_C`（C 键） |
| `PAD_BTN_DPAD_*` | 十字键 | 十字键（帽子开关展开） | `NS2_BTN_DPAD_*` |
| `PAD_BTN_L4` / `PAD_BTN_L5` / `PAD_BTN_R4` / `PAD_BTN_R5` | 侧键、精英手柄 2 的背键 P1-P4 | DualSense Edge 背键（L4 / R4） | `NS2_BTN_GL` / `NS2_BTN_GR`（同侧合并） |
| `PAD_TRIGGER_L2` / `PAD_TRIGGER_R2` 模拟量 ≥ 2048（50%） | LT / RT | L2 / R2 | `NS2_BTN_ZL` / `NS2_BTN_ZR` |
| `PAD_AXIS_LX` / `LY` / `RX` / `RY`（0-4095，中位 2048） | 左右摇杆（有符号 16 位） | 左右摇杆（单字节） | 12 位打包的摇杆字段 |

左侧小键与分享类两颗位同帧双置时只出减号（`ns2_from_pad` 折掉截图位）：串流虚拟手柄
（Sunshine/Moonlight）把一颗 View 键双写成 SHARE+触摸板按下以兼容 PC 游戏，直译会让
一次按键在主机侧同时点亮减号与截图。

DS4 / DS5 的触摸板按下可以改成加减键（「DS4、DS5 设置」页与串口 `ds` 命令，两项都持久化在 NVS）：
「触摸板映射加减键」开着时按先触发的半区发
`PAD_BTN_TOUCHPAD`（减号）或 `PAD_BTN_OPT`（加号），「截图键」关掉时这一路改发减号、默认发截图。
两项都只在 PS 家族的触摸板按下位上生效，位置取不到时退回截图键那一档；
键位在按下那一刻定一次、按住期间不变，改写由 `pad/ds_behavior.c` 在数据面每拍完成。

家族表按系列拆在 `firmware/main/pad/layouts/` 下，契约与注册表是 `pad/layout.h` / `pad/layout.c`。
表按（家族、Report ID、连接方式、PID、报告长度）定位偏移，
家族先按 VID 判定、厂商 VID 分不开的按模块的型号表判定，长度限定的行排在通用的行前面：
PS 系的 DS3、DS4 与 DualSense 有线都报 0x01，同一个 Report ID 下按 PID 分行；
Xbox 蓝牙的四轴、扳机与按键偏移对所有长度一致，只有精英手柄 2 的背键位按长度分三行。
各族偏移的取值与核对状态列在 [controller-ps.md](controller-ps.md)、[controller-xbox.md](controller-xbox.md)、
[controller-xinput.md](controller-xinput.md) 与 [controller-ns1.md](controller-ns1.md) 的「核对状态」，
Steam 原生布局整族走兜底并在能力位里标记。

手柄组合键 L1+R1+L3+R3 按住 300 ms 会捕获输入、转为屏幕操控：
判定在私有格式层完成（`firmware/main/dp/dp_ui.c`），家族表只需要把 L1/R1/L3/R3 映射到 `PAD_BTN_L1/R1/L3/R3`，既有与将来的布局都自动可用。
dp_task 在捕获的那一刻先向主机补发一帧全松开（清掉 `raw_len` 与 `native_lang`，避免同代透传把旧按键带过去），
其后按原来的上报节奏续发同一份中性帧——主机按稳定不跳号的上报流判断链路健康，整段停发会被它判成手柄离线。
玩家输入从捕获起一点不上行，同时十字键与圆圈键映射成 UI 按键位（`firmware/main/dp/dp_ui.h` 的 `DP_UI_BTN_*`），
经 50 ms 一轮的状态快照交给界面。

```mermaid
stateDiagram-v2
    [*] --> Forward
    Forward: 转发玩家输入
    Captured: 只发中性帧，方向键与圆圈键驱动 UI
    Forward --> Captured: L1+R1+L3+R3 按住 300 ms（先补发一帧全松开）
    Captured --> Forward: 再按同样的组合（恢复转发）
```

界面侧把各页与底栏的焦点资格绑在「自己是当前页、且没有弹窗盖住」上：`App` 往下传 `interactive`，各页按它决定自己进不进焦点名单，
隐藏页的可点元素因此不会留在名单里。焦点序号由固件写入，焦点环只在 `pad-active` 为真时可见（操控窗口打开，或最近 4 秒内有按键）。
圆圈键的确认与触摸点按最终都落到 `action(name, value)` 这一个回调上；方向键 / WASD 与串口 `key ui` 走同一条通路。
模式状态由 `dp_ui_active()` 经状态快照写进界面，调试页、串口 `ui [on|off]` 与 `key ui` 都能在不插手柄时进出。

## 界面图元与控件

界面只有三类东西：自己定位的 `Rectangle`（底色、圆角、边框）、`Text`（画的是构建期烘好的字形位图）与由它们拼出的控件，
外加只用于底图的 `Image`。控件复用集中在 `ui/src/components.slint`：

- **布局**：需要成排/成列、还要按内容撑开的区域用 `HorizontalLayout` / `VerticalLayout` 加 `alignment`；
  页面主体按设计稿量测值给绝对位置（例如底栏三格按整屏 240 均分），改设计稿要同步改 `ui/host/tests/` 的断言。
- **文本**：字号只取 12 / 14 / 16 / 24；中文由 NotoSansSC 烘制，图标与转圈走字形（`Icon` 控件指定字形字体）。
- **图标**：单色图标是 Material Symbols 的字形，写成 `Icon { glyph: "\u{e30c}"; size: ...; tint: ...; }`，
  码点对照表在 [ui/README.md](../ui/README.md)；不要退回 SVG，缩放后上屏会糊。
- **可点控件**：控件外壳是 `Rectangle`，触摸区是内部的 `DragArea`（继承 `TouchArea`，并写入全局 `Drag` 供拖动层判定）；
  `activated` 落到页面回调，再由 `App` 转成 `action(name, value)`。
- **焦点**：页面按 `focus-index` 画 2px 焦点环（`theme.slint` 的 `focus-ring`），并且只在 `interactive` 为真时进焦点名单。
- **底图**：卡片与底栏的两张 SVG 在构建期光栅化成位图（`ui/assets/*.svg`），运行期只做拷贝与混合。
- **页面组织**：八个页面（发布构建七个，开发构建多一个调试页）在 `App` 里全部常驻，
  `visible` 由 `page` 属性决定，切页瞬时完成、没有建树成本；页表与每页的焦点项数写在
  `function focus-count-for` 里（固件按这个行数走焦点）。新增页面在 `app.slint` 的卡片里加一个槽位并同步该函数。
  新增可聚焦行同样同步它。
- **只改属性，不加节点**：列表长度这类会来回变的东西不要用 `for` 动态增删，写固定的槽位再按属性显示/隐藏；
  颜色与尺寸也不要接过渡动画（`animate` 只用在翻页提示与拖动跟手上），过渡期间那片区域每帧都要重画。
- **拖动与滚动**：卡片区域只保留跟手平移（拖动层 `DragArea` 汇入全局 `Drag`，位移封顶 ±24，
  抬手按位移阈值或甩动方向切页）；页面自身不做滚动——内容按设计稿一次性摆好。

## 构建产物映射

```mermaid
flowchart TB
    Source["ui/src/*.slint + ui/assets/（字体与 SVG）"]
    Build["build.rs：构建期编译"]
    Generated["OUT_DIR 下生成的 Rust 代码"]
    Cargo["cargo --target xtensa-esp32s3-none-elf"]
    Lib["firmware/build/esp-idf/slint_ui/cargo/<br/>xtensa-esp32s3-none-elf/release/libslint_ui.a"]
    Link["ESP-IDF 链接进应用镜像"]

    Source --> Build
    Build --> Generated
    Generated --> Cargo
    Cargo --> Lib
    Lib --> Link
```

生成物只有两处：cargo 的 `target/`（`.gitignore` 已忽略）与 `firmware/build/`。仓库里没有手写的嵌入源、字节数组或同步脚本。
宿主用例走另一条链：`ui/host/build.rs` 编译同一份 `.slint`，产物落在 `ui/target/`。

## ESP-IDF 运行时生命周期

core 与界面的分工是：`firmware/main/ui/ui_service.c` 装配状态快照并分发动作，`ui/slint_ui/ui_host.c`
负责面板/触摸/背光装配、启动画面与渲染任务；固件核心对界面框架零依赖（契约见 `ui_service.h`）：

```mermaid
flowchart TB
    Task["remapad-ui 提供者任务<br/>面板 / 触摸 / 背光 + 启动画面"]
    Start["remapad_slint_ui_start(hooks, poll, action)"]
    Platform["建平台：整帧 PSRAM 缓冲 + 内部 RAM 行带缓冲"]
    Window["建 240 × 280 窗口并设平台"]
    App["App::create + 首轮状态快照"]
    Render["首帧整屏渲染"]
    Submit["按 damage 折 48 行行带提交"]
    Loop["事件循环：定时器 / 动画 / 触摸 / 重绘"]

    Task --> Start
    Start --> Platform
    Platform --> Window
    Window --> App
    App --> Render
    Render --> Submit
    Submit --> Loop
```

带 UI 构建里固件经组件 C ABI 传两类东西：`remapad_slint_hooks_t`（面板提交与触点采样）和两个回调
（状态快照，落到 core 的 `ui_service_fill_state`；动作分发，落到 `ui_service_handle_action`）。
界面侧不持有任何固件指针；快照里的字符串是只读借用，回调返回后即作废。
无 UI 构建（`REMAPAD_UI=OFF`）里同一组生命周期入口由 `main/ui/ui_stub.c` 顶上：
面板/触摸/背光不初始化、屏幕熄灭，OTA 健康门槛在启动时直接放行。

## 输入抽象

界面有两条输入：**触摸**（用户手指）与**手柄按键位**（组合键捕获期间的屏幕操控）。

- 触摸由固件经 `hooks.touch_sample` 提供，返回逻辑像素坐标；平台把它转成指针移动、
  按下、抬手的事件序列（按住期间只补移动，抬手时补一次移出）。
- 手柄按键位来自数据面的 `dp_ui_buttons()`（只在组合键捕获期间非零，含十字键、圆圈键与肩键等价出的左右），
  由 `host.rs` 转成焦点移动、翻页与确认。
- CST816T 是单点触摸，坐标用逻辑像素；息屏（背光关闭）期间平台整段跳过采样——画面不可见，触点只剩误触，
  亮屏由 PWR 键或命令承担。板卡引脚见 [hardware.md](hardware.md)。
- 平台不采样模拟量：界面没有需要模拟输入的地方。

USB→NS2 的高频状态留在产品数据面，界面只经状态快照看低频结果（配对、电量、USB 角色与玩家灯）。

## 渲染抽象

平台的职责是把界面画成像素并提交，不负责面板时序：

1. 软件渲染器只画本帧 damage 区域，结果写进整帧 RGB565 缓冲（PSRAM，240 × 280）。
2. 每条 damage 矩形按 48 行切分，逐行拷进内部 RAM 的行带缓冲（240 × 48）。
3. `hooks.transfer` 把行带交给面板驱动：字节序转换、SPI EDMA 与等待完成都在驱动里。
4. 整帧缓冲同时是截图通道的数据源：`remapad_slint_ui_frame()` 给只读指针，`remapad_slint_ui_copy_frame()` 整屏拷贝。

面板初始化失败时平台照常渲染，只是没有可提交的去处；S3 没有 P4 那类 PPA，全程纯软件渲染（整数运算）。

## 调度抽象

界面侧只有一条任务：`remapad-ui` 提供者任务（64 KB 栈、内部 RAM、钉在 CPU1）承载面板 / 触摸 / 背光初始化、
启动画面、`remapad_slint_ui_start` 与之后的事件循环。事件循环每轮：推进定时器与动画 → 采样触摸 → 按需重绘并提交 →
让出 CPU（有动画时 16 ms 一档、最长 100 ms；不让出会把循环拉成自旋并饿死空闲任务）。

界面状态与动作都在这个任务的上下文里交换：界面框架按单线程前提编译（`unsafe-single-threaded`），
跨任务只走原子量（帧缓冲地址、trace 帧数、截图请求）与 `bridge` 的命令队列。
状态快照由一条 50 ms 的界面 `Timer` 驱动，手柄按键处理与周期统计都挂在这一拍上。
core 侧的控制面另有一条服务任务（`remapad-bridge`，每 50 ms 泵一次命令队列与配对状态机），
它不依赖界面存在——无 UI 构建里命令通路照常工作。

## 硬件扩展边界

新增外设（面板、触控、按键与模拟量、USB host、BLE、背光、电池）直接用 ESP-IDF 或对应官方驱动，在对应的 BSP / 数据面边界接入：
面板与触控归 `drivers/`，USB 与音频触觉归 `usb/`，BLE 归 `ble/`，各自的约束写在对应目录的文件头。
驱动提供的事实经 [hardware.md](hardware.md) 记录，需要暴露给界面的读数再加进 `remapad_ui_state_t` 的状态快照字段——
没有真实硬件事实时不要提前声明字段。
协议字段与配对流程见 [controller-switch2.md](controller-switch2.md)，文档里的实验性结论不等于已完成的互操作保证。

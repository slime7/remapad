# Remapad 核心概念与领域抽象

本文档记录 Remapad 的两条数据路径：PocketJS 显示 UI 路径，以及 USB→NS2→BLE 控制器路径。PocketJS 包格式、C ABI、UI 输入编码和渲染指令不在项目内复制；需要调整时应以 PocketJS 官方 schema、组件头文件和 ESP-IDF 示例为准。NS2 协议、广播、GATT、HID 报告和配对内容见 [controller.md](controller.md)。

## 领域术语表

| 术语 | 含义 |
| :--- | :--- |
| **Pocket manifest** | `ui/pocket.json`，描述应用入口、框架、视口和 capability 要求。 |
| **Host profile** | `firmware/pocket.host.json`，描述设备实际提供的 ESP32-S3 host 能力和显示事实。 |
| **Pocket package** | `.pocket` 单文件包，包含 manifest 对应的构建计划、JavaScript、PAK 和目标 variant。由官方 CLI 生成，由 `pocketjs_package` 读取。 |
| **PAK** | PocketJS 资源包，承载样式、baked font atlas、图片等运行时资源。由 `pocketjs_ui_qjs_feed_pak` 提供给 binding。 |
| **Guest** | `pocketjs_guest` 创建的 QuickJS 执行环境，负责运行编译后的 JavaScript。 |
| **UI core** | `pocketjs_ui_core` 维护的 retained UI 节点、资源句柄、动画和 frame view。 |
| **UI binding** | `pocketjs_ui_qjs` 将 `globalThis.ui`、`globalThis.__pak` 和 UI turn 连接到 guest/core。 |
| **Damage region** | 一帧中需要重新光栅化的逻辑矩形；renderer 将其输出为 full-width RGB565 strip。 |
| **Host BSP** | 项目自己的 ESP-IDF 硬件层，负责面板、DMA、触控、按键、电源和其他外设。 |
| **USB input** | 由 ESP32 USB host 接收的外部输入报告，先进入产品数据面，不直接进入 PocketJS。 |
| **接收段（input/）** | 输入通路的第一段：桥接帧的编解码与串口分帧、USB-Serial/JTAG 的唯一读取者、实现 `dp_source_t` 的输入源。 |
| **处理段（pad/）** | 输入通路的第二段：家族布局表把各家手柄报告解析成私有格式 `pad_state_t`（按键按位置语义、摇杆归一、能力位）。 |
| **转换段（target/）** | 输入通路的第三段：目标编码器 `pad_target_t` 把私有格式编码成具体目标家族的报文，现役实现为 `target/ns2/`。 |
| **桥接帧** | PC 与设备之间的分帧载荷：帧头 `A5 5A` 加版本、类型、槽位、序号、长度字段，再跟载荷与 CRC16，与 CLI 文本共用一根 USB-Serial/JTAG；承载输入帧（ATTACH/REPORT…）、OTA 升级帧（`0x30`-`0x33`）与 PING 探测帧。 |
| **OTA 会话（ota/）** | 升级通道的固件侧：`ota_session` 负责帧队列、非阻塞分派、flash 写入与重启，`ota_proto` 是纯逻辑的序号判定、窗口应答、4 KB 聚合与超时；镜像写进非运行分区，校验通过后切启动分区（见 [ADR 0022](adr/0022-ota-over-bridge-frames-with-rollback.md)）。 |
| **NS2 report encoder** | 将私有手柄状态（`pad_state_t`）编码为目标 NS2 手柄的 USB/BLE 报告，位于 `firmware/main/target/ns2/`。 |
| **BLE controller peripheral** | 对 NS2 主机执行广播、GATT 服务、输入通知、输出命令和配对状态管理的 ESP32 外设角色。 |
| **Product control plane** | UI bridge 与固件控制面，用于低频状态、配置、配对操作和诊断；不承载高频输入报告。 |

## 应用清单与 host profile

应用和设备各自声明事实，官方 resolver 在构建时验证兼容性：

```mermaid
flowchart LR
    App["ui/pocket.json<br/>应用声明"] --> AppEntry["entry / framework"]
    App --> AppView["logical viewport"]
    App --> AppReq["requires"]
    App --> AppEnh["enhances"]

    Host["firmware/pocket.host.json<br/>设备声明"] --> HostPlatform["platform = esp-idf"]
    Host --> HostAbi["host ABI / tickHz"]
    Host --> HostView["physical / logical viewport"]
    Host --> HostPres["presentation / density"]
    Host --> HostCap["capabilities"]
```

- `requires` 是应用运行所必需的能力，host 不提供时构建应失败。
- `enhances` 是应用可以利用但不应作为最低运行条件的能力。
- `capabilities` 只能填写固件确实会提供的能力。当前 Remapad profile 声明 `text.glyphs.baked` 与 `input.touch`；后者随触摸 BSP（CST816T 采样，见 [ADR 0007](adr/0007-esp-lcd-panel-touch-bsp.md)）接入一并加入，按键和模拟量能力仍不在 profile 中。
- profile 的 canonical hash 会进入构建计划和 package variant，运行时 `pocketjs_package_select` 会校验目标、ABI、tick、视口、density、presentation 和 profile hash。
- 当前设备的逻辑和物理视口均为 `240×280`。生成的 JavaScript bundle 可能仍包含官方 framework 的 `SCREEN_W = 480`、`SCREEN_H = 272` fallback 常量；它们不是设备 profile 的显示事实，也不应手动修改生成产物。ESP-IDF host 按 package contract 创建 `pocketjs_ui_core`，并通过 `globalThis.ui.__viewport` 发布 `240×280`；构建计划和运行时 frame 才是设备尺寸的校验依据。

触摸预览页可以在浏览器中提供真实触点，浏览器 host 与设备 host 各自把输入交给同一套框架语义：预览页把指针事件转换为触摸帧，设备端由 `drivers/touch.c` 把 CST816T 采样填入 `sample_input`。

## 最终产品控制器数据面

USB 到 NS2 BLE 的目标链路如下：

```mermaid
flowchart TB
    Bridge["PC 手柄（已实现）<br/>pc/ 桥接程序读原始报告并转发"]
    Host["USB host 手柄（未实现，M5）<br/>手柄插在板卡上：USB mux 切换 + HID 接收"]
    Recv["input/ 接收段<br/>帧解码 / 串口分帧 / dp_source_t 输入源"]
    Parse["pad/ 处理段<br/>家族布局表解析 + 归一 → pad_state_t"]
    Encode["target/ 转换段<br/>pad_target_t → NS2 报告编码（target/ns2/）"]
    Ble["BLE 广播 / GATT / 输入通知 / 输出命令"]
    Session["NS2 主机的连接与配对"]
    Feedback["pad_feedback_t：主机反馈（震动 / 玩家 LED / 触觉采样）"]

    Bridge -->|"桥接帧，USB-Serial/JTAG"| Recv
    Host -. "IN 64B 中断传输（HID 报告 + Report ID）" .-> Recv
    Recv -->|pad_report_t| Parse
    Parse --> Encode
    Encode --> Ble
    Ble --> Session
    Ble -.-> Feedback
    Feedback -. 输入侧投递 .-> Recv

    classDef planned stroke-dasharray: 5 5
    class Host planned
```

实线是已经落地的路径（PC 侧插手柄，经桥接帧进来），虚线是尚未实现的部分：USB host 直插（手柄插在板卡上）需要先做 USB mux 实验与 VBUS 供电确认，方案见 [usb-input-plan.md](usb-input-plan.md)，待办见 [ROADMAP.md](ROADMAP.md) M5。两条路径在这里汇合，之后共用 `pad/` 与 `target/` 两段，解析与映射只有一份。

这条链路需要保持低延迟和确定性：

- USB 接收、解析、状态快照和 BLE 发送应使用 ESP-IDF 原生驱动、任务和队列。
- NS2 报告编码应按 [controller.md](controller.md) 的型号、Report ID、摇杆打包、震动输出和字节序实现，并用实机抓包验证。
- BLE manager 负责厂商广播字段、GATT service/characteristic、通知订阅、回连、唤醒和配对状态机；配对凭证通过 NVS 等持久化层保存。凭证（每条 6B 主机 MAC + 16B LTK）是百字节级小 blob、低频写（仅配对成功时一次），与 PHY 校准同住 NVS：NVS 的掉电一致性与磨损均衡正是为这类数据设计，不需要也不应迁往 storage 分区（其定位是大块通用数据）。凭证按身份分槽、每个身份保留最近 2 条（对齐 controller.md §7.4 的两条存储模型：主机公网地址与私有第二接口地址），按 MAC 覆盖、超出丢弃最旧，配对新主机是追加而非覆盖，因此不常驻「解除配对」入口。
- 广播状态位（厂商数据偏移 0x0B）是主机唯一的唤醒判据，三种形态由 `ns2_adv_payload()` 统一成型（`firmware/main/target/ns2/ns2_adv.c`，主机端用例钉住黄金字节）：发现广播不带主机地址、状态 0x00；回连广播带最近一条凭证里的主机地址、状态 0x00；唤醒广播同样带地址、状态 0x81。**已配对身份在未连接期间常驻唤醒形态**——主机醒着但停在非配对页面时只认 0x81，0x00 回连形态不被采纳，这正是「只有主机停在配对页面才连得上」的解法；未配对身份或配对流程期间发发现广播，配对流程之外静默（真机没配对时不广播），开机若当前形态从未配过任何主机则自动进入配对流程。形态决策是 `ns2_adv_choose_mode(paired, pairing_requested, steady)` 纯函数，取舍、副作用（设备通电且未连接时待机主机会被唤醒）与回退开关见 [ADR 0024](adr/0024-ns2-steady-wake-adv-and-pairing-key.md)。
- 连接间隔由主机下发（常为 4 单位即 5 ms，低于 BLE 规范的 7.5 ms 下限）：ESP32-S3 控制器由 `CONFIG_BT_CTRL_BLE_MIN_CONN_INTERVAL_ENABLE` 允许亚规范间隔（sdkconfig.defaults 显式开启，最低 3.75 ms），固件只观测不主动请求（GAP 连接更新事件记日志，`ns2_session_status().conn_itvl` 供串口 `link` 打印）——NimBLE 主机侧按规范拒绝 itvl < 6 的请求。输入报文被主机采用的真正门槛有两个：一是 **0x0C/0x04（启用特性）**——未启用的链路（握把/顺序页的快捷回连形态，跳过完整握手、反复重发 0x0C/0x02）即使 itvl=4 也不采用输入，输入通知从启用后才发送，已订阅却迟迟不启用的会话由休眠看门狗断开重连（15 秒未启用，每次上电至多 3 次，`ns2_adv_dormant_link()` 判定）；二是**稳定不跳号的上报流**——上报按 15 ms 分频（dp 仍 5 ms 采样，`DP_SEND_DIV=3`，对齐已验证实现的 `HID_REPORT_INTERVAL=15ms`），以 5 ms 从任务侧灌 63B 通知会打爆发送队列（实测近半数因 mbuf 耗尽被丢、计数器跳号）。`0x0E` 运动数据长度必须非零（按 40 字节零值占位，与已验证实现一致）——板卡无 IMU，块内容保持全零。
- flash 写入期间 cache 被禁用，而 PocketJS owner task 的栈在 PSRAM——从该任务直接执行任何 flash 写都会在禁缓存窗口访问 PSRAM 并触发 cache 异常重启（「停止配对即重启」的根因）。凭证等持久化写一律收敛到 `ble_creds` 的内部 RAM 栈写任务：各任务只更新内存表并投递快照，新增持久化需求必须沿用同一模式。
- `ui/src/bridge/` 与 `firmware/main/bridge/` 只适合承载低频的模式切换、开始/停止配对、连接状态、电池和诊断消息。传输层已接通：guest 侧 `HardwareDriver` 经 `globalThis.__nativeBridge.postMessage(json)` 发命令（由 `pocketjs_host.c` 在 mount 后、eval 前用 `pocketjs_guest_quickjs_install_once` 注入的 native surface 接收并入队），owner task 每帧 `js_bridge_service()` 处理队列并用 `pocketjs_guest_eval` 调 `__onNativeBridgeMessage(json)` 回发应答/事件；入队与出队都在 owner task 上，无锁。PWR 按键与串口 CLI 等非 owner task 上下文经 `js_bridge_submit_command` / `js_bridge_post_event` 的外部队列转移（guest eval 只允许在 owner task 上执行）。协议以 `ui/src/bridge/protocol.ts` 为准，命令包含 hello/getSystemStatus/setBacklight/setScreenPower/setUsbRole/getControllerConfig/setControllerConfig/startPairing/stopPairing/unpair/pressLr/debugKey/powerOff/reboot，事件包含 ready/systemStatus/backlightSet/screenPowerSet/screenPowerChanged/usbRoleSet/usbRoleChanged/controllerConfig/controllerConfigSet/pairingResult/unpairResult/pressLrAck/pairingStateChanged/playerLedChanged/debugKeySet/powerOffAck/rebooting/powerOffBlocked/error。玩家序号灯（主机 Command 0x09 下发的 4 位掩码）由 `ns2_session_player_leds()` 按活跃会话汇总，随 `systemStatus.playerLed` 与变化时的 `playerLedChanged` 事件供首页四格指示灯显示（bit0-3 对应从左到右四格，无主机时为 0，断开连接自动回落）。用户设置（背光亮度、手柄身份类型与配色、上报固件版本）由 `firmware/main/config/app_config.c` 持久化到 NVS（内部 RAM 栈提交任务，与 ble_creds 同一模式），开机恢复；USB 角色不落盘。桥接（otg）角色开发期临时禁用防误操作：UI 移除选项、后端静默跳过（不报错），协议保留该值。
- USB 角色（`usbRole`: device=插电脑 COM 口，host=插手柄）目前只由固件记录并如实上报 `usbRoleActive`，且只在本次运行有效（不写 NVS），重启回到串口；USB OTG PHY 切换属于数据面，未接入前任何代码都不触碰 RTC_CNTL USB mux，复位后永远回到默认的 USB-Serial/JTAG（COM 设备模式），"重启回 COM 模式"因此天然成立。
- 配对与连接状态已接入真实 BLE 会话（NimBLE 手柄外设，进度见 [ROADMAP.md](ROADMAP.md)）：配对页的「开始」等价于真机按住配对键——bridge 的 `startPairing` 先断开当前主机再进发现广播等新主机搜索，`stopPairing` 退出流程（已配对回常态广播，未配对静默）；配对新主机也可以完全自动：开机时当前形态没有任何凭证就自动进入配对流程，凭证拿齐（JoyCon 组合要求左右都拿到）后由 tick 自动退出流程。解除配对由显式 `unpair` 命令完成（清除 NVS 凭证并回到配对流程），UI 不暴露该入口。广播时机由凭证决定——已配对发唤醒广播等主机回连、未配对发发现广播等主机搜索、配对流程之外静默，断开后按同一规则自动恢复；连接建立时广播随连接自然停止。已连接但始终停留在握手等待态的主机（手机/PC 自动回连）由空闲超时主动断开（3 秒无协议活动，主机毫秒级初始化序列不受影响）；主机连接地址是随机地址，不能按 OUI 识别。配对成功的判定走协议证据——主机初始化/0x15 握手完成（或凭证匹配回连）记为主机已注册，NVS 凭证则是重启后仍成立的持久化证据，两者独立；配对六态由此实时推导并经 `pairingStateChanged` 推送，配对流程期间底栏不再锁定（流程可能长期挂着等新主机）。Command 0x15 私有配对与 NVS 凭证见 [controller.md](controller.md) 与 [ADR 0010](adr/0010-nimble-ble-controller-stack.md)。电池电压由 `battery.c` 真实采样（BAT_ADC=GPIO1 / ADC1_CH0，分压 3:1 还原为 VBAT，静置电压—容量表折算百分比），充电状态没有可测量的引脚，是按采样电压趋势推断的值，限制见 [ADR 0020](adr/0020-battery-adc-sampling-and-charge-inference.md)。
- 手柄身份与配对凭证按 `ns2_identity_t`（Pro / JoyCon L / JoyCon R）分槽（`ble_creds`，NVS v2 格式，旧单表记录迁移进 Pro 槽）：切换手柄类型后主机眼中是另一台设备，配对信息不共用。Pro 为单连接双 PDU 广播；JoyCon 组合为左右双连接（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`），各占一个广播实例与静态随机 AdvA（`ns2_identity_adv_addr()`：按身份的固定盐把公共地址扩散成另一串字节，再置成静态随机形态——最高字节 bit7/bit6 置一，最低位右置一、左清零；Pro 与左右两只的地址互不共用字节序列，避免主机把不同形态认成同一台或把两只认成一只，同一芯片上结果稳定；NimBLE 每实例地址仅支持 RANDOM，与真机 public 形态不同，主机兼容性待实机验证），序列号 / PID / 出厂块（0x7E40 与 0x13000）按连接身份提供，输入报告按身份切分左右半边（左：L/ZL/减号/截屏/十字键/左摇杆，右：A/B/X/Y/C/R/ZR/Home/右摇杆，NFC 状态只在右手柄保留）。身份由会话层显式传给传输层，广播地址不再反推身份——芯片地址最低位奇偶不定，反推会把左只认成右只。切换手柄类型（或配色）等价于旧手柄断电、新手柄上电：写入新身份后先断开现有连接、按新身份重算配对状态与出厂块，由断连回调恢复广播——未配对的新身份自动进配对流程（需在主机配对页配一次），已配对身份直接发唤醒广播等回连。配对页「按下 LR」（`pressLr` 命令）是手动兜底；未配对期间固件会自动注入 L+R 120ms 并每 3 秒重试（见下一条调试注入）。会话层面向连接分槽（最多 2 个），桥接命令 / 应答 / 通知都带连接上下文，并维护每槽的报告计数与链路快照（`ns2_session_status()`，串口 `link` 与日志取用）；`controllerConfig` 应答带 `addresses`（pro / left / right，显示序大写十六进制，host 未同步时为空串），手柄设置页的身份信息行取用它。
- 配对对外的心智模型是「开机即连接，界面只管配对键」：配过主机就常驻唤醒广播自动回连（主机停在任意页面都能连上），从未配过则开机自动进入配对流程，主机侧配对记录在首次连接握手时完成，用户不需要在屏幕上做任何确认动作；屏幕上的配对页只用于观察会话状态、按配对键配新主机或退出流程，主机 Grip / 手柄顺序界面只用于调整手柄顺序与确认 JoyCon 已配对。JoyCon 组合保持左右两条独立连接与两条独立凭证（各占一个广播实例），未配对期间固件自动注入 L+R 120ms 并每 3 秒重试——主机靠同时按下的 L 与 R 把两只认成一对，屏幕 UI 不做合并成单个设备的展示。
- 主机推送的手柄固件更新按「接受并假装升级」处理：0x0018 升级数据块写入被计数接收，静默 10 秒视为完成，上报版本（app_config 持久化，0x10 查询与两个出厂块共用）递增落盘；真实升级协议无公开文档，需抓包后再对齐（见 controller.md §12）。
- 调试注入是控制面进入数据面的唯一低频通道，采样与编码仍由数据面任务独立完成，不引入高频路径（`firmware/main/dp/dp_source.c`）：按键注入经 `dp_source_inject()` 叠加一次按下并按时长自动释放（默认 250ms，配对 L+R 约 1s，对应主机 Grip/顺序界面的配对确认动作，上限 60s，`dp_source_inject_release()` 可提前释放），摇杆注入经 `dp_source_inject_stick()` 给出持续电平（0-4095，两侧独立，未设定的一侧沿用输入源的值，`dp_source_inject_stick_reset()` 回中并解除注入）。注入是合成的最后一步：按键叠加在合成按键上，设定过的摇杆覆盖合成摇杆。按键名表由 `dp_source_key_lookup()` 提供，串口 CLI 与主机端用例共用；UI 调试页「按键指令」区走 bridge 的 `debugKey`（仍是 a / home / lr 三个键；「唤醒 HOME」在注入 HOME 的同时请求重连——已连接则断开让主机按唤醒广播重连，主机休眠时按键也进不去，只有 0x81 广播能把它叫醒）。串口 `link` 命令按身份打印链路快照（`ns2_session_status()`）：对外广播地址、连接句柄、当前连接间隔（`itvl`，4 = 5 ms）、会话状态、报告格式、已开启的通知通道、特性启用位（`feat`，收到 0x0C/0x04 后为 1，输入通知自此才发送与计数）、已发送报告数、凭证条数与广播形态（`adv`，取 `wake` / `reconnect` / `discovery` / `off`），配对、组合、连接间隔与分侧上报都能在串口上对账；按键变化另有数据面限频日志（`buttons 0x… -> 0x…`，最小间隔 200 ms）。
- 输入与输出已解耦成三段稳定接口（`firmware/main/dp/dp_source.h`、`firmware/main/pad/pad_state.h`、`firmware/main/target/target.h`，边界见 [ADR 0021](adr/0021-input-path-three-stage-layering.md)）：新增输入设备（桥接 PC、将来的 USB 手柄、调试注入）只需实现 `dp_source_t` 并注册，首个注册源拥有摇杆/扳机/触摸/运动与设备标识字段，后续源叠加按键，调试注入最后叠加；私有格式 `pad_state_t` 是上下段之间的唯一接缝，目标侧 `target_send_pad()` 按注册的 `pad_target_t` 编码（现役 `target/ns2/`，内部仍调 `ns2_output_send()`，可只填需要输出的按键）。主机下发的震动 / 玩家 LED / 触觉采样被 ble_session 解析为结构化事件（`ns2_rumble_event_t` 等），在反馈监听者里归一到 `pad_feedback_t` 并回发桥接帧；投递到插入手柄的动作在后续里程碑实现。电池经 `battery.c` 唯一入口 + `ns2_output_set_battery` 随报告上发；amiibo 镜像经 `ns2_output_amiibo_stage` 预置（传输方式待定），Report 0x09 的 NFC 状态字节随预置汇报。USB host 直插的推进方案见 [usb-input-plan.md](usb-input-plan.md)。
- USB 高频输入不应经过 JSON bridge，也不应等待屏幕刷新或 JavaScript guest 执行。

## 输入通路：接收 / 处理 / 转换

输入通路按三段划分（取舍见 [ADR 0021](adr/0021-input-path-three-stage-layering.md)）：`input/` 只把字节变成「原始报告 + 设备标识」，`pad/` 只把原始报告变成私有格式并收敛家族差异，`target/` 只把私有格式编码成目标报文。三段之间是单向数据流：新增一种手柄只加家族表一行，新增一个目标（例如将来的 NS1）只加一个 `pad_target_t` 实现。

```mermaid
flowchart LR
    subgraph PC["PC（pc/ 桥接程序）"]
        HID["手柄 HID 报告"] --> BR["bridge.py：原始报告 + 设备标识"]
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
        uint8 player_led
        uint8 haptic_sample
    }

    pad_report_t --> pad_state_t : pad_state_from_report
    pad_state_t --> pad_target_t : target_send_pad
    pad_feedback_t ..> pad_state_t : 反向链路（目标 → 输入设备）
```

- 按键位按位置固定、键名沿用 PS（`PAD_BTN_TRIANGLE` 上、`PAD_BTN_CIRCLE` 右、`PAD_BTN_CROSS` 下、`PAD_BTN_SQUARE` 左）：Xbox 与 Nintendo 的 A/B/X/Y 标签位置不同，用 PS 名可以避免「A 到底指哪个键」的混淆，家族表把各家的物理键填进对应位置；背键与静音键（目标侧作 C 键）用扩展位占位。
- 四轴与双扳机统一为 0-4095 整数、摇杆中位 2048，Y 轴统一成「上为正」，8% 死区在解析段套用并把剩余行程重新铺满；扳机保持模拟量，是否数字化由目标决定。
- `caps` 标注这一帧里哪些字段真的来自设备（运动、触摸板、模拟扳机、背键、麦克风、电池、震动）；型号未识别时回落 Xbox 布局并置 `PAD_CAP_FALLBACK_LAYOUT`，结果仍可用但字段可能错位。
- 目标只消费自己 `caps` 范围内的字段：不在集合里的部分（IMU、触摸板、麦克风）不映射，能力集合变化时提示一次，不逐帧刷日志。
- 桥接帧与 CLI 文本共用一根 USB-Serial/JTAG：接收侧校验 CRC、失步时只丢一个字节继续扫描，非帧字节原样交回命令行解析，因此桥接跑着的时候串口 CLI 照常可用。

帧类型（固件侧定义在 `firmware/main/input/input_frame.h`，PC 端在 `pc/link.py` 镜像一份）：

| 类型 | 方向 | 载荷 |
| :--- | :--- | :--- |
| `0x01` ATTACH / `0x02` DETACH | PC → 设备 | 8 字节设备标识（家族 / 连接方式 / VID:PID / Report ID / 报告长度） |
| `0x10` REPORT | PC → 设备 | 设备标识 + 原始报告（最多 64 字节） |
| `0x20` FEEDBACK | 设备 → PC | 左右震动使能与强度、玩家灯、触觉采样 |
| `0x30` OTA_BEGIN | PC → 设备 | `ROM1` + 镜像字节数（u32 小端） |
| `0x31` OTA_DATA | PC → 设备 | 块序号（u16 小端）+ 最多 200 字节镜像数据；帧内 `slot=1` 标记该窗口的末帧 |
| `0x32` OTA_END | PC → 设备 | 空 |
| `0x33` OTA_ACK | 设备 → PC | 状态 + 错误码 + 期望序号（u16 小端）+ 已收字节（u32 小端）；对 BEGIN 的应答末尾再附 16 字节运行版本 |
| `0x7F` PING | 双向 | 协议版本号（1 字节） |

解码器按线格式上限 255 字节收帧，报文帧仍按 72 字节语义校验（8 字节设备标识 + 最多 64 字节报告）。OTA 帧由 `input_link` 交给 `ota/ota_session`，PING 由 `input_link` 直接应答，其余交给 `input_source`；升级协议、流控与回滚门槛见 [ARCHITECTURE.md](ARCHITECTURE.md) 的「OTA 升级通路」。

PC 手柄到 NS2 主机的完整时序（映射表把家族差异收敛在 `pad/`，所以桥接路径与将来的 USB host 直插路径共用后面两段）：

```mermaid
sequenceDiagram
    autonumber
    participant PC as pc/bridge.py
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
    loop 每 5 ms
        DP->>SRC: dp_source_sample()
        SRC->>DEV: pad_state_from_report()
        DEV-->>SRC: pad_state_t
        DP->>TGT: target_send_pad()
        TGT->>HOST: ns2_output_send() → BLE 输入通知
    end
    HOST->>DP: 主机反馈（震动 / 玩家 LED / 触觉采样）
    DP->>PC: FEEDBACK 桥接帧（本轮只打印）
    PC->>RECV: DETACH 帧（拔线或退出）
    RECV->>SRC: 状态回静置，按键不卡住
```

家族与目标的按键对应关系（按位置对齐，因此 Xbox 的物理 A 与 PS 的 Cross 都落在 `PAD_BTN_CROSS`、再到 `NS2_BTN_B`）：

| 私有格式（位置语义） | Xbox 物理键 | PS 物理键 | Steam（原生布局） | NS2 目标 |
| :--- | :--- | :--- | :--- | :--- |
| `PAD_BTN_CIRCLE`（○ 右） | B | Circle | 未登记，走兜底 | `NS2_BTN_A` |
| `PAD_BTN_CROSS`（✕ 下） | A | Cross | 未登记，走兜底 | `NS2_BTN_B` |
| `PAD_BTN_TRIANGLE`（△ 上） | Y | Triangle | 未登记，走兜底 | `NS2_BTN_X` |
| `PAD_BTN_SQUARE`（□ 左） | X | Square | 未登记，走兜底 | `NS2_BTN_Y` |
| `PAD_BTN_LB` / `PAD_BTN_RB` | LB / RB | L1 / R1 | 未登记，走兜底 | `NS2_BTN_L` / `NS2_BTN_R` |
| `PAD_BTN_LSTICK` / `PAD_BTN_RSTICK` | 左/右摇杆按下 | L3 / R3 | 未登记，走兜底 | `NS2_BTN_LSTICK` / `NS2_BTN_RSTICK` |
| `PAD_BTN_TOUCHPAD`（触摸板按下） | View（select） | 触摸板按下 | 未登记，走兜底 | `NS2_BTN_MINUS`（减号） |
| `PAD_BTN_OPT`（选项） | Menu | Options | 未登记，走兜底 | `NS2_BTN_PLUS`（加号） |
| `PAD_BTN_HOME`（主页） | 西瓜键 | PS 键 | 未登记，走兜底 | `NS2_BTN_HOME` |
| `PAD_BTN_SHARE`（分享） | 分享键（Series 手柄） | Create / 分享 | 未登记，走兜底 | `NS2_BTN_CAPTURE`（截图） |
| `PAD_BTN_MUTE`（静音） | 无 | DualSense 静音键 | 未登记，走兜底 | `NS2_BTN_C`（C 键） |
| `PAD_BTN_DPAD_*` | 十字键 | 十字键（帽子开关展开） | 未登记，走兜底 | `NS2_BTN_DPAD_*` |
| `PAD_BTN_L4` / `PAD_BTN_L5` / `PAD_BTN_R4` / `PAD_BTN_R5` | 侧键 / 背键 | DualSense Edge 背键（L4 / R4） | 未登记，走兜底 | `NS2_BTN_GL` / `NS2_BTN_GR`（同侧合并） |
| 扳机模拟量 ≥ 2048（50%） | LT / RT | L2 / R2 | 未登记，走兜底 | `NS2_BTN_ZL` / `NS2_BTN_ZR` |
| `PAD_AXIS_LX` / `LY` / `RX` / `RY`（0-4095，中位 2048） | 左右摇杆（有符号 16 位） | 左右摇杆（单字节） | 未登记，走兜底 | 12 位打包的摇杆字段 |

家族表的偏移初值取自公开资料，落地时用 `pc/bridge.py --dump` 抓原始报告核对后再固化（DualSense 蓝牙的 0x31 行已按 DualSense Edge 实测核对并单独登记）；Steam 原生布局未抓包，暂时走兜底并在能力位里如实标记。

## UI 图元与资源

`ui/src/App.tsx` 使用 PocketJS Vue Vapor 的 `<View>`、`<Text>` 和 `<Image>` 等图元：

- `<View>` 提供嵌入式布局、背景、边框、间距和 focusable 交互。
- `<Text>` 使用构建期收集的字符集和 baked font atlas；字号应使用 PocketJS 支持的 Tailwind 插槽。Inter 未映射的码点（中文等）经应用目录 `fonts.json` 声明的回退字体面（当前为 Noto Sans SC）烘焙进同一图集。
- `<Image>` 通过资源名称引用 PAK 中的图像；图片在构建期处理，不在 ESP32 上解析 SVG。
- `createSpriteAnimation` 只描述资源帧选择，实际资源仍由官方编译器和 PAK 管理。
- 长文案放不进可视区时用 `ui/src/components/MarqueeText.tsx`（自定义横向滚动文本）：框架的单行 `Text` 不自动换行，组件按「静止 2 秒 → 匀速左移到底 → 到底停留 1 秒 → 跳回起点」循环，放得下则全程静止；可视宽度由调用方以逻辑像素传入（框架不回读布局），滚动相位取 `virtualNow()`，文本宽度经 `getOps().measureText(text, slot)` 量取，宿主不提供该操作时退回静态文本。
- 页面组织：`ui/src/App.tsx` 在首次渲染里一次挂完七个页面，首屏（第一次 commit）只在全部建树完成后提交，等待期由固件启动画面覆盖（原生建树约每节点 50 ms，选型见 [ADR 0016](adr/0016-mount-all-pages-before-first-frame.md)）。App 没有页面容器层也没有待挂队列，切页由每个页面根节点翻转 `hidden`（`props.active()`）完成，新增页面直接写在 JSX 里并自行负责 `hidden`。
同页会来回切换的状态用 `hidden` 收起而不是条件渲染：手柄设置页的身份信息行与状态提示行都常驻（每行就是一个文本节点，标签与值同节点、省掉行容器：Pro 两行「序列号 / MAC」，JoyCon 四行「左序列号 / 左 MAC / 右序列号 / 右 MAC」，右只两行翻 `hidden` 收起）。运行期增删节点在实机上很贵（建树约 50 ms/节点），除条件渲染外，JSX 里的 `.map(...)` 只要在构造列表时读了响应式状态，该状态一变就会卸载整段旧节点、重建等价的新节点（手柄设置页的序列号行原先正是这种写法：一次形态切换要重建 13 个原生节点、15 次插入、5 次移除，按每节点约 50 ms 计实机要阻塞半秒以上，固件应答回写同一状态还会再走一遍）；列表应取自模块级常量，可变字段留给子组件按属性绑定（模式页 `RoleCard`、手柄设置页的身份信息行都是这个写法）。切换类样式也不要带 `transition-*`：过渡期间该区域每帧都要重画（选项卡两张卡约 2.2 万像素、实机每帧 11–14 ms，共 9 帧），观感是慢半拍。滚动页在滚动列末尾放 `components/BottomPlaceholder.tsx` 垫高（`BOTTOM_PAD_H = 80`）；页面内容高度由各页静态估算后传给 `usePageScroll`（框架不回读布局），估算误差由这段垫高的兜底余量吸收。页面能否滚动由调用方一次声明（`usePageScroll(active, scrollable, contentH)`），不再从内容高度推导；滚动边界硬夹住（`overscroll: 0`），拖拽不会拉出边界，抛掷落点越界的会被改写成到边界的补间，没有橡皮筋，滚到底就停。全应用只注册一个纵向手势，识别区域由当前接管滚动的页面给出：非当前页或不可滚动的页让 region 返回 null，本帧不接管新触点（官方优先级即注册顺序，逐页注册会互相抢 claim，dispose 重注册又会取消进行中的触点）。

入口保持官方 Vue Vapor 形式：

```jsx
import { mount } from '@pocketjs/framework/vue-vapor';
import Hero from './App';

mount(() => <Hero />);
```

固件 `pocketjs_ui_qjs_mount` 会在应用 eval 前安装 `globalThis.ui` 和 `globalThis.__pak`；应用入口不再手动传入 PAK，也不依赖私有 prelude。

## 构建产物映射

```mermaid
flowchart TB
    Source["Vue Vapor JSX + pocket.json + host profile"]
    Compiler["PocketJS 官方 compiler"]
    JS["remapad-ui.js"]
    Pak["remapad-ui.pak"]
    Pocket["remapad-ui.pocket"]
    Embed["pocketjs_embed_package / compile_app"]
    Output["firmware/build/pocketjs/remapad/<br/>C 与汇编嵌入文件、生成头文件（均为 CMake 产物）"]

    Source --> Compiler
    Compiler --> JS
    Compiler --> Pak
    Compiler --> Pocket
    Pocket --> Embed
    Embed --> Output
```

`firmware/build/pocketjs/remapad/` 中的 C/汇编嵌入文件和生成头文件都是 CMake 产物。项目不应再出现手写的 PCKT 解析、字节数组或 `app_pocket.h` 同步脚本。

## ESP-IDF 运行时生命周期

`firmware/main/pocketjs_host.c` 使用官方 C API，顺序与官方 ESP-IDF smoke 示例保持一致，但创建、mount、eval 和逐帧 turn 都在同一个产品 task 上完成：

```mermaid
flowchart TB
    Package["embedded .pocket bytes"]
    Open["pocketjs_package_open"]
    Select["pocketjs_package_select（host contract）"]
    Guest["guest_create（QuickJS）"]
    Core["ui_core_create（contract viewport）"]
    Mount["ui_qjs_create → feed_pak → mount → guest_eval"]
    Turn["remapad-pjs owner task<br/>sample_input → pocketjs_ui_turn → after_turn"]
    Plan["prepare damage plan"]
    Strip["render_strip（RGB565）"]
    Transfer["panel transfer by BSP"]
    Commit["commit / abort"]

    Package --> Open
    Open --> Select
    Select -->|"borrowed JS + PAK views"| Guest
    Guest --> Core
    Core --> Mount
    Mount --> Turn
    Turn --> Plan
    Plan --> Strip
    Strip --> Transfer
    Transfer --> Commit
```

包中的 JavaScript 和 PAK 都是借用视图，必须在 guest、binding 和 package 销毁前保持可读。生成的 package header/assembly 由 CMake 管理，因此不会发生 UI 与固件手动复制不一致的问题。

## 输入抽象

官方 `pocketjs_ui_input_t` 是一次 UI turn 的输入快照，包含：

- `buttons`：设备按键位图。
- `analog_x`、`analog_y`：左模拟量。
- `touches`、`touch_count`：当前触点数组。

输入采样属于 host/BSP，不属于 PocketJS 应用包。当前实现由 owner task 的 `sample_input` 回调返回零按键、零模拟量、零触点；屏幕是触摸屏，接入后应把 CST816T 的采样转换为官方 `pocketjs_ui_touch_t` 触点数组，触点 `id` 在同一按压期间保持稳定、坐标使用逻辑像素（板卡引脚见 [hardware.md](hardware.md)）。USB→NS2 的高频状态应留在产品数据面，不应为了驱动 UI 而重新设计 PocketJS runtime 的输入协议。

## 渲染抽象

官方 RGB565 renderer 的职责是从 UI frame view 生成像素，不负责面板控制：

1. `pocketjs_rgb565_prepare` 生成 damage plan 并开始目标事务。
2. 对每个逻辑 damage region，分配或复用一个 full-width、region-height 的 RGB565 strip。
3. `pocketjs_rgb565_render_strip` 将 strip 写入调用方提供的缓冲区；容量必须精确匹配物理宽度乘以 region 高度。
4. BSP 将 strip 传给面板 DMA，所有传输成功后调用 `pocketjs_rgb565_commit`。
5. 任一渲染或传输失败时调用 `pocketjs_rgb565_abort`，不要提交不完整帧。

ESP32-S3 没有本项目使用的 P4 PPA 加速器，因此 renderer 使用官方软件 RGB565 路径。当前仓库只验证 strip 生成和事务，不宣称已经完成 ST7789 传输。

## 调度抽象

当前由产品自己的 `remapad-pjs` owner task 承担调度：它按 host profile 的 `tickHz` 驱动 UI turn，同时承载 guest 的创建、mount 和 eval。它只负责调度、输入采样回调与帧消费，不拥有输入驱动或显示设备。

不使用官方 `pocketjs_runner` 的原因是任务栈的宿主：`pocketjs_runner_config_t` 只能指定栈大小，FreeRTOS 任务栈始终由 IDF 从内部 RAM 分配，而 mount 需要的连续 C 栈空间超出内部 RAM 的可用容量。owner task 通过 `xTaskCreatePinnedToCoreWithCaps` 把栈放在 PSRAM。

**创建 guest 的任务和执行 UI turn 的任务必须是同一个。** QuickJS 的栈守卫以下限 `stack_top - stack_size` 判断溢出，而 `stack_top` 取自创建 runtime 时的栈指针，`JS_UpdateStackTop` 在官方组件中没有被调用。若 turn 换到别的任务执行，守卫量的是别人的栈，溢出不会被拦截。改动调度时这一点不能破坏；同时任务栈容量必须大于 guest 的 `stack_limit`。

## 硬件扩展边界

产品 BSP 以后可以包含：

- ST7789 初始化、方向/偏移配置和 SPI/并口 DMA；
- 触控控制器、GPIO 按键和模拟量采样；
- USB host、输入报告解析和 NS2 报告编码；
- BLE 广播、GATT、配对/回连、震动命令和电源管理；
- 背光、电池和其他设备状态；
- 将上述事实映射到 `pocket.host.json` capabilities。

这些功能应直接使用 ESP-IDF 或对应官方驱动，并在对应的 BSP/data-plane 边界接入。没有真实硬件事实时，不在 UI manifest 或 host profile 中提前声明能力。协议字段和配对流程以 [controller.md](controller.md) 为参考，不应把文档中的实验性结论当作已完成的互操作保证。

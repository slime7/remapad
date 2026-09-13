# 阶段路线图与执行计划

本文件是阶段执行计划的跟踪入口：记录当前阶段的里程碑拆解、验收标准、风险与"明确不做"清单，并随里程碑落地更新状态。产品定位与阶段划分见 [VISION.md](VISION.md)，架构约束见 [ARCHITECTURE.md](ARCHITECTURE.md)，控制器协议依据见 [controller.md](controller.md)。

**状态标记约定**：`未开始` / `进行中` / `已完成`。每个里程碑对应一次或多次独立提交，完成后在状态列回填，并在相应文档（按 [AGENTS.md](../AGENTS.md) 维护映射表）同步实现现状。

## Phase 1 — 官方 host 链路与屏幕 BSP（已完成）

双工作区架构、PocketJS 官方 ESP-IDF 组件全链路、240×280 触摸预览、ST7789V2 面板提交、CST816T 触摸采样、背光 PWM、低频 bridge（v0.2.0，7 条命令）、OTA 终局分区表（[ADR 0009](adr/0009-ota-storage-flash-layout.md)）。详见 [VISION.md](VISION.md) 当前阶段一节。

## Phase 2 — 控制器数据面：BLE 手柄链路先行，USB 输入殿后

**顺序决策**：三大环节中 NS2 编码是纯软件枢纽；BLE 私有协议（GATT/配对/连接参数）风险最高且必须依赖 Switch 2 实机迭代，故 **BLE 链路先行**，用合成输入源打通上报链路，USB host 输入作为阶段末尾里程碑接入。全程依据 [controller.md](controller.md)，凡逆向结论以实机验证为准，不符处记录勘误。

架构不变量：高频数据面（输入采样 → 编码 → BLE notify）由原生任务承载，**绝不经过 PocketJS turn或 JSON bridge**；bridge 只承载低频状态与控制命令。

### M1 — NS2 编码核心（纯软件，无硬件依赖）　状态：已完成（上板日志比对随 M2 联调执行）

新建 `firmware/main/ns2/`：

- [ ] `ns2_state.h`：规范化手柄状态结构（按键位图、双摇杆 0–4095、扳机、电池/电源、连接状态）。
- [ ] `ns2_report.c`：Input Report 0x09（Pro Controller 2 专用，BLE 日常格式）编码器；Input Report 0x05 备选；摇杆 12 位紧凑打包/解包（controller.md §5.3）；BLE 省略 / USB 带 Report ID 的差异处理。
- [ ] `ns2_frames.c`：8 字节指令帧头构造与应答（0x02 SPI 读写、0x03 初始化、0x09 玩家 LED、0x0A 触觉采样、0x0C 特性掩码、0x10 版本查询）；Command 0x15 配对四步帧构造（§3）。

**验收**：编码输出以日志十六进制比对 controller.md 字节布局；摇杆 pack/unpack 往返一致；帧头各字段（Direction 0x91/0x01、Transport、Status/ACK）与 §6.1 一致。

### M2 — BLE 手柄外设骨架（NimBLE + 广播 + GATT + 上报循环）　状态：代码完成（idf.py build 通过，UUID/广播字节已机械核对；实机验收待烧录联调）

新建 `firmware/main/ble/` 与 `firmware/main/dp/`（模块边界见 [ADR 0011](adr/0011-controller-dataplane-module-boundary.md)，栈选型见 [ADR 0010](adr/0010-nimble-ble-controller-stack.md)）：

- [ ] sdkconfig 启用 NimBLE（BLE-only），NimBLE 主机堆评估分配到 PSRAM（与 PocketJS guest 共存，内部 RAM 预算核查）。
- [ ] `dp/dp_task.c`：独立数据面任务 + 输入源抽象；输入源先接合成测试源，静置无按键（最初的自动按键遍历已按实机测试需要移除，按键输入改由调试页 `debugKey` 注入触发）。
- [ ] `ble/ble_controller.c`：广播构造（标准发现 / 回连 / 唤醒三变体，31 字节 = Flags 3B + 厂商数据 28B：Company ID 0x0553、VID 0x057E、PID 0x2069、状态位）；GATT 两大服务按 §4 精确 UUID/handle 落表（Input 0x000A/0x000E + CCCD 0x000B/0x000F、Output 0x0012、Command 0x0014、应答 0x001A + CCCD 0x001B、复合输出 0x0016），全部 Write Without Response。
- [ ] `ble/ble_session.c`：连接初始化时序（§10.2：0x001B CCCD → 0x07/0x01 握手 → 版本/出厂信息/校准应答 → LED → 0x0C 特性配置 → 0x000F CCCD → 5–15ms notify 循环）。

**验收（真机分档）**：① nRF Connect 可见广播且厂商数据逐字节一致、GATT 结构匹配；② Switch 2 "更改 Grip/顺序"界面发现并连接；③ 握手应答帧日志符合 §10.2 时序，调试注入的按键在主机侧可见变化。

### M3 — 配对、回连与凭证持久化　状态：代码完成（build 通过，配对算法向量校验通过；实机配对/回连验收待烧录联调）

- [ ] `ble_session.c` 实现 Command 0x15 四步配对（MAC 交换 → LTK = A1 XOR B1（固定常量 `5C F6 EE 79 2C DF 05 E1 BA 2B 63 25 C4 1A 5F 10`）→ AES-128-ECB 反序挑战（mbedtls）→ 确认），全程不触发 SMP；实机观察主机侧 SMP 行为并按文档拒绝。
- [ ] 引入 `nvs_flash`，凭证存 NVS（[ADR 0009](adr/0009-ota-storage-flash-layout.md) 约定 BLE 配对密钥存 NVS）：主机 MAC + 16B LTK，记录语义对齐 controller.md §7.4 的 0x1FA000 结构（1B 数量 + 40B 记录项）。
- [ ] 回连广播（主机 MAC 反序）+ 会话状态机（Idle → Advertising → Connected → WaitPairing → NormalOperation，§10.1）；唤醒广播（状态位 0x81）为可选项，时间不够顺延。
- [ ] Output Report 0x02 震动解析与日志确认（板卡无震动马达，最终转发给 USB 源手柄，属 M5）。

**验收**：Switch 2 完成配对、摇杆/按键实时上报可用；断电重启后免配对回连成功；震动输出帧解析正确。

### M4 — 控制面真实化（轻量收口 BLE 侧）　状态：已完成（bridge v0.3.0，模拟状态机删除，实机显示待联调）

- [ ] `firmware/main/bridge/js_bridge.c`：删除模拟配对状态机；`startPairing/stopPairing`、`pairingStateChanged/pairingResult` 事件、`systemStatus` 的 controller/model 字段改接真实 BLE 会话；电池保持占位（真实 ADC 放 M5 顺带）。
- [ ] UI 侧预期近零改动：配对页/首页/状态栏已消费这些命令与事件，换真实数据源即生效。

**验收**：屏幕上配对流转、已连接状态与实机一致；`pnpm run check` / `lint` / `build` 通过。

### M5 — USB host 输入接入（阶段末尾）　状态：未开始

新建 `firmware/main/usb/`：

- [ ] USB mux 切换实验定案：`usb_new_phy()`（OTG + HOST）+ USB-Serial-JTAG 让出、日志切 UART0（GPIO43/44）；结论回填 [hardware.md](hardware.md) 并出 ADR。
- [ ] VBUS 5V 供电路径确认（hardware.md 挂起项，决定 host 模式能否给手柄供电，必要时调整方案）。
- [ ] `usb_host_hid.c`：host lib 安装、复合设备枚举（跳过 Vendor Bulk / 音频接口）、claim HID 接口、IN 64B 接收 + OUT 发送队列；解析带 Report ID 的 0x05/0x09 输入 → 规范化状态 → 接入 dp_task 输入源；BLE 下发的震动/LED 经 OUT 反向转发。
- [ ] `usbRole` 命令真实化（切换策略预计"确认后重启进入 host 模式"，实验后定）；`battery.c` 真实 ADC（GPIO1）顺带接入。

**验收**：NS2 手柄插板 → Switch 2 收到真实手柄输入；主机震动可传到手柄；模式页 host 角色真实生效。

## 阶段补强（2026-09，M5 前的控制面与数据面收口）　状态：已完成（实机联调随 M2/M3 验收执行）

- [x] **输入/输出解耦**：`dp/dp_source.c` 输入源抽象（注册制，合成源现役，USB/桥接源按 [usb-input-plan.md](usb-input-plan.md) 预留）+ `ns2/ns2_output.c` NS2 输出封装（`ns2_output_send` 按需按键构建报告、会话格式自适应、计数器内聚；主机震动/LED/触觉采样解析为结构化事件分发；`battery.c` 唯一电池入口；amiibo 镜像预置 API 与 Report 0x09 NFC 状态字节预留）。
- [x] **用户设置持久化**：`config/app_config.c`（NVS，内部 RAM 栈提交任务）：背光亮度、连接模式、手柄身份（类型 + 配色）随命令落盘、开机恢复；息屏不跨重启。
- [x] **手柄设置页（UI）**：类型 Pro（默认）/JoyCon 组合（HBW10067/HCW10068 序列号展示）、颜色选择预留；固件侧身份应用到出厂块与广播 PID（JoyCon 单连接以 L 身份，实机验证前仅记录）。
- [x] **PWR 按键**（`drivers/pwr_key.c`）：短按息屏/亮屏；长按 3-6s 切连接模式（device ↔ host）；桥接 otg 双端禁切（COM 断开保护），SYS_EN 电源保持待电源 BSP。
- [x] **串口 CLI**（`console/cli.c`，主控制台切 USB-Serial/JTAG）：status/key/backlight/screen/mode/pairing/reboot 行命令，经 bridge 外部队列走同一路径；PC 端 `scripts/uartctl.py`。

## 阶段补强 2（2026-09 第二批：双按钮导航回归与 JoyCon 组合双连接）　状态：固件代码完成（Pro 路径实机验证通过；JoyCon 双连接与固件更新伪装的实机验收待真实主机联调）

- [x] **底部导航回归双按钮**：状态（宽键）+ 设置（方键）恢复原版结构，配对/模式/系统/调试归入设置页菜单；PocketJS 原生表面不支持单角圆角，按钮背景整体画进 SVG（宽键拆 2 个 64×64 半图），圆角曲线必须用三次贝塞尔（烘焙器对 path 圆弧指令光栅化错位），贴屏幕角的一角 24、其余三角 16。
- [x] **JoyCon 组合双连接**：左右两只各占一个广播实例（静态随机 AdvA——NimBLE 每实例地址仅支持 RANDOM，与真机 public 形态不同，需实机验证）、独立序列号（命名规则 + 校验位，`ns2_serial.c`）/PID（0x2067/0x2066）/出厂块，凭证按身份分槽（`ble_creds` v2，旧记录迁移 Pro 槽）；输入按身份切分（左半/右半）；配对页「按下 LR」（`pressLr` 命令）触发组合确认；Pro 单连接路径保持原有双 PDU 广播。
- [x] **固件更新伪装**：版本号统一来源（app_config 持久化，默认 1.6.1）并补齐 0x7E40 块上报（此前缺失，主机读不到版本疑似更新提示诱因）；0x0018 升级数据块「接受计数 + 静默 10s 视为完成」，版本递增落盘（假装升级）。
- [x] **PWR 长按提示音**：`drivers/buzzer.c`（GPIO42 LEDC tone），长按到 3 秒短鸣一声提示可松开。
- [x] **桥接（otg）静默跳过**：UI 移除选项、后端不报错仅忽略（开发期临时禁用防误操作），协议保留 otg 值。

## 阶段补强 3（2026-09 第三批：启动画面、跑马灯与导航几何）　状态：代码完成（UI 预览已确认；实机启动时序与观感待烧录目视）

- [x] **固件启动画面**（`firmware/main/boot_splash.c`）：UI 就绪前面板自绘主题底色 + 几何手柄标记 + 阶段进度条（4 个按键点随阶段轮转高亮），启动画面落屏后即点亮背光，把「BLE 秒级可用、屏幕黑好几秒」的落差收掉；每阶段推进一次进度，失败时进度条转 error 色，UI 首帧提交后交出屏幕并释放缓冲（常驻按键点与进度条两块小缓冲合计约 5 KB PSRAM，240×280 全屏缓冲约 131 KB 在整帧提交后即释放）。见 [ADR 0012](adr/0012-firmware-boot-splash-before-ui.md)。
- [x] **横向滚动文字组件**（`ui/src/components/MarqueeText.tsx`）：放得下全程静止；溢出时「静止 2 秒 → 40 px/秒匀速左移到底 → 尽头停留 1 秒 → 跳回起点」循环；首个使用点为手柄设置页类型说明行。
- [x] **底部导航几何落定**：容器边距 8、按钮圆角 8，状态键左下角 32、设置键右下角 32；宽键由左右半图 + 中段贴图拼成并恢复 `grow`（152 宽），图标居中覆盖层与底部避让高度（80）同步。
- [x] **配对心智模型文案同步**：配对无需打开界面（开机即自动广播），主机 Grip / 手柄顺序界面只用于排序与确认 JoyCon，JoyCon 保持左右两条独立连接（不改固件行为）。

## 阶段补强 4（2026-09 第四批：USB 角色不落盘、提示音通路修复与启动进度时间轴）　状态：代码完成（待实机确认提示音与启动画面观感）

- [x] **USB 角色不持久化**：`app_config` 不再读写 NVS 里的角色字段，开机恒为串口（device）；`setUsbRole` 只改运行时值，UI 模式页补「重启后回到串口」。
- [x] **PWR 长按提示音修复**：蜂鸣器与背光此前共用 LEDC 定时器/通道，提示音把背光占空比一起清零（表现为长按关屏且不响）；`drivers/buzzer.c` 改用独立定时器与通道。串口 CLI 新增 `beep [ms]` 便于不带按键自检。
- [x] **启动进度时间轴**：进度条改为「阶段权重表 + 阶段内经过时间」由启动画面自带的动画任务推进，实测 `guest_eval` 约 16 秒、其余阶段合计不到 0.5 秒，权重按实测填写；四个按键点随进度轮转，右侧按键点由贴边改为内收 12 px。
- [x] **uartctl 日志读取**：`scripts/uartctl.py log [--seconds N] [--reset] [--raw]`，只读设备日志并加相对时间前缀，`--reset` 先复位以抓完整启动日志。

## 阶段补强 5（2026-09 第五批：节点缩减与页面按需挂载）　状态：代码完成（UI 预览逐像素比对无差异；实机启动时序待烧录确认）

- [x] **节点缩减**：删掉 7 层页面容器与 2 处导航覆盖层，状态栏角色标签与信息行占位合并进既有节点（`justify-between` 顶开），外观逐像素不变、原生建树成本每页省数帧。
- [x] **页面按需挂载**：首帧只挂壳、状态栏、底栏与首页，其余页面首次进入时才建外层容器；切页只翻转各页根节点的 `hidden`，已建页面不再重建。
- [x] **逐帧自顶向下填充**（`ui/src/hooks/useProgressiveMount.ts`）：页内用 `useMountCursor` 每帧放行一个填充块，外层容器先出现、兄弟块自上而下逐块补齐，把单帧阻塞从整页秒级压到单块数百毫秒。见 [ADR 0014](adr/0014-page-mount-on-demand-progressive-fill.md)。
- [x] **首页加载提示复用**：首页留白处的 spinner 改由 `pagesFilling()` 驱动，页面分帧填充期间可见。

## 阶段补强 6（2026-09 第六批：同页节点不重建与统一垫高）　状态：代码完成（UI 预览逐像素比对通过；实机待烧录确认）

- [x] **手柄设置页改固定节点**：序列号两行结构固定，非当前形态的那行用 `hidden` 收起，Pro/JoyCon 来回切换不再卸载重建节点；内容高度按最大形态估一次，颜色卡改按真实高度（含 mt-2 与色块行）计入，滚到底不再遮住色块。
- [x] **模式页状态提示行固定**：`roleMessage` 行常驻，无提示时收 `hidden`。
- [x] **首页加载提示常驻**：spinner 与文案改由 `hidden` 控制，避免加载状态切换时卸载重建；收起时不绑定动画帧，否则常驻的 `Image` 每 3 帧换一次图，实测空闲 `avg_turn_us` 从 5.1 ms 涨到 13.9 ms。
- [x] **底部垫高统一**：垫高从每页末尾一个占位节点改成滚动列统一样式（`theme` 的 `scrollColumn*` 内含 `pb-[120]`，数值即 `BOTTOM_PAD_H`），每页少一个节点；`usePageScroll` 在内容变矮时把越界位置收回边界。

## 风险与依赖

- **协议精度风险**：controller.md 全部为逆向结论，广播/GATT/配对/时序均需实机迭代；预留真机调试窗口，不符处在 controller.md 增补勘误小节。
- **内存预算**：NimBLE host + BLE controller 与 QuickJS guest（4MB JS 堆）共存；内部 RAM 当前约 360KB 空闲，必要时 NimBLE 堆切 PSRAM（ADR 0010）。
- **连接间隔主导权在主机**：5–10ms 区间由 Switch 2 作为 central 发起，外设侧需确保接受且上报循环跟上节奏（§11）。
- **VBUS 供电未知（M5 门禁）**：板卡唯一 Type-C 兼任烧录/日志/输入，host 模式下 PHY 切换会失去 COM 口，且 VBUS 5V 供电路径待原理图确认；M5 起步前先完成两项硬件确认。
- **实机条件**：Switch 2 主机、NS2 手柄（Pro Controller 2）、USB-C 数据线/OTG 转接、UART 串口适配器均已具备。

## 本阶段明确不做

桥接角色（电脑输入 → NS2）、OTA、amiibo/storage 分区、IMU/RTC/蜂鸣器外设、输入映射 UI、UI 基础组件库（Phase 2 另一支线，另行安排）。

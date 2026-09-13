# 阶段路线图与执行计划

本文件只保留**尚未完成**的计划：待办里程碑、验收标准、风险与"明确不做"清单。已落地的实现不留记录——现状写进 [VISION.md](VISION.md)、[ARCHITECTURE.md](ARCHITECTURE.md)、[ABSTRACTIONS.md](ABSTRACTIONS.md) 与 [docs/adr/](adr/README.md)，里程碑落地时更新这几处，不在本文件追加完成记录。产品定位与阶段划分见 [VISION.md](VISION.md)，架构约束见 [ARCHITECTURE.md](ARCHITECTURE.md)，控制器协议依据见 [controller.md](controller.md)。

**状态标记约定**：`未开始` / `进行中` / `已完成`。每个里程碑对应一次或多次独立提交。

## Phase 1 — 官方 host 链路与屏幕 BSP　状态：已完成

## Phase 2 — 控制器数据面：BLE 手柄链路先行，USB 输入殿后

**顺序决策**：三大环节中 NS2 编码是纯软件枢纽；BLE 私有协议（GATT/配对/连接参数）风险最高且必须依赖 Switch 2 实机迭代，故 **BLE 链路先行**，用合成输入源打通上报链路，USB host 输入作为阶段末尾里程碑接入。全程依据 [controller.md](controller.md)，凡逆向结论以实机验证为准，不符处记录勘误。

架构不变量：高频数据面（输入采样 → 编码 → BLE notify）由原生任务承载，**绝不经过 PocketJS turn 或 JSON bridge**；bridge 只承载低频状态与控制命令。

### M1–M4 — 编码核心、BLE 链路、配对凭证与控制面　状态：已完成

### M2/M3 — 实机验收　状态：未开始

- **广播与 GATT**：nRF Connect 可见广播且厂商数据逐字节一致（Company ID `0x0553`、VID `0x057E`、PID `0x2069`），服务与句柄表符合 [controller.md](controller.md) §4。
- **连接时序**：Switch 2「更改 Grip/顺序」界面能发现并连接，握手应答符合 §10.2 时序（0x001B CCCD → 0x07/0x01 握手 → 版本/出厂信息/校准应答 → LED → 0x0C 特性配置 → 0x000F CCCD → notify 循环）。
- **输入上报**：调试页注入的按键在主机侧可见变化，摇杆与按键连续上报不丢帧。
- **配对与回连**：Command 0x15 四步配对在实机完成且不触发 SMP；断电重启后免配对回连成功。
- **JoyCon 组合**：左右两个广播实例均被主机发现并连接，分槽凭证回连正常。
- **震动输出**：Output Report 0x02 解析正确（板卡无马达，最终转发给 USB 源手柄，属 M5）。
- **UI 观感**：启动画面时序、切页与滚动观感在实机确认。

### M5 — USB host 输入接入（阶段末尾）　状态：未开始

新建 `firmware/main/usb/`：

- [ ] USB mux 切换实验定案：`usb_new_phy()`（OTG + HOST）+ USB-Serial-JTAG 让出、日志切 UART0（GPIO43/44）；结论回填 [hardware.md](hardware.md) 并出 ADR。
- [ ] VBUS 5V 供电路径确认（hardware.md 挂起项，决定 host 模式能否给手柄供电，必要时调整方案）。
- [ ] `usb_host_hid.c`：host lib 安装、复合设备枚举（跳过 Vendor Bulk / 音频接口）、claim HID 接口、IN 64B 接收 + OUT 发送队列；解析带 Report ID 的 0x05/0x09 输入 → 规范化状态 → 接入 dp_task 输入源；BLE 下发的震动/LED 经 OUT 反向转发。
- [ ] `usbRole` 命令真实化（切换策略预计"确认后重启进入 host 模式"，实验后定）；`battery.c` 真实 ADC（GPIO1）顺带接入。

**验收**：NS2 手柄插板 → Switch 2 收到真实手柄输入；主机震动可传到手柄；模式页 host 角色真实生效。

## Phase 3 — UI 性能与启动时间　状态：未开始

UI 的每帧成本集中在整幅软件 RGB565 光栅化与每帧 draw list 重建上（见 [adr/0017](adr/0017-display-path-and-scroll-frame-budget.md)），启动成本集中在 guest 侧 bundle 的解析与执行（`guest_eval` 约占 16 秒）。两个里程碑分别针对这两处瓶颈；应用侧能动的只有「每帧画多少像素」和「包怎么加载」，因此都先与官方 PocketJS 上游确认可行边界。

### M1 — 带动画部位只做局部刷新　状态：未开始

- [ ] 目标：一帧只重画真正变化的区域，尤其是带过渡与动画的部位（选项卡选中态、跑马灯、加载动画），把滚动与过渡帧从整幅 240 × 280 光栅化降到动画区域本身。
- [ ] 待确认：官方 renderer 的 damage plan 粒度（`pocketjs_rgb565_prepare` 给出的矩形是否已按节点与动画目标收敛）与 render-to-texture 方案的 PSRAM 预算。
- [ ] 验收：选项卡切换、列表滚动、跑马灯这类动画部位的单帧重绘面积等于该部位本身，滚动帧稳定 60 Hz（当前整幅帧渲染约 50 ms、滚动 17–20 fps）。

### M2 — 缓存 JavaScript 字节码，缩短重启时间　状态：未开始

- [ ] 目标：bundle 在构建期预编译成字节码随包缓存到 Flash，开机只做加载与校验，省掉 `guest_eval` 的解析执行时间（当前 UI 首帧约 18 秒）。
- [ ] 待确认：官方编译器是否支持输出字节码、QuickJS 版本升级时缓存如何失效、缓存体积与存放位置（`storage` 分区或包内变体）。
- [ ] 验收：UI 首帧时间显著下降且渲染结果不变；更换固件或组件后缓存自动失效，不出现旧字节码导致的崩溃。

## 风险与依赖

- **协议精度风险**：controller.md 全部为逆向结论，广播/GATT/配对/时序均需实机迭代；预留真机调试窗口，不符处在 controller.md 增补勘误小节。
- **内存预算**：NimBLE host + BLE controller 与 QuickJS guest（6.5 MB JS 堆）共存；内部 RAM 当前约 360 KB 空闲，必要时 NimBLE 堆切 PSRAM（ADR 0010）。
- **连接间隔主导权在主机**：5–10 ms 区间由 Switch 2 作为 central 发起，外设侧需确保接受且上报循环跟上节奏（§11）。
- **VBUS 供电未知（M5 门禁）**：板卡唯一 Type-C 兼任烧录/日志/输入，host 模式下 PHY 切换会失去 COM 口，且 VBUS 5V 供电路径待原理图确认；M5 起步前先完成两项硬件确认。
- **实机条件**：Switch 2 主机、NS2 手柄（Pro Controller 2）、USB-C 数据线/OTG 转接、UART 串口适配器均已具备。

## 本阶段明确不做

桥接角色（电脑输入 → NS2）、OTA、amiibo/storage 分区、IMU/RTC/蜂鸣器外设、输入映射 UI、UI 基础组件库（Phase 2 另一支线，另行安排）。

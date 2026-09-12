# 0010 — BLE 手柄外设采用 NimBLE 栈

- 状态: active
- 日期: 2026-09-12
- 替代: 无

## 背景

Phase 2 路线图（docs/ROADMAP.md）M2-M4 进入 BLE 手柄外设实现。ESP32-S3 只支持 Bluetooth LE 5.x、无经典蓝牙控制器；docs/controller.md 的参考实现（第 10.3 节）基于 NimBLE，社区 Switch 2 模拟实践亦然。NS2 使用私有配对协议，发起或响应标准 SMP 会被主机直接断连，安全由应用层 Command 0x15 配对承担；连接间隔需接受主机（central）主导的 5-10ms 区间；固件还需与 PocketJS guest（4MB JS 堆）共存，内部 RAM 空闲约 360KB，栈与堆预算紧张。

## 决策

采用 ESP-IDF 内置 NimBLE host + BLE-only controller：不启用 Bluedroid；SMP 保持关闭，不主动发起安全请求，收到主机 SMP 请求按协议拒绝；广播由应用层构造 31 字节原始 ADV 数据（Flags + 26 字节厂商数据）；连接参数接受主机请求并约束在 5-10ms 区间；NimBLE 主机内存优先分配到 PSRAM（CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL），保障 guest 堆与内部 RAM 预算。

## 考虑的方案

- NimBLE：BLE-only 轻量主机，controller.md 参考实现同栈，SMP 可干净关闭（采纳）
- Bluedroid：ESP-IDF 双栈主机，资源占用更大，本场景无经典蓝牙需求（否决）
- 自研或第三方 BLE 主机栈：成本与风险不可行（否决）

## 影响

- 正面：内存占用小、与协议参考实现同栈、SMP 禁用路径清晰、实机对照资料充足。
- 约束：NimBLE 版本随 ESP-IDF 6.x 内置版本锁定；连接间隔实际由主机主导，需实机验证外设侧接受策略；31 字节广播无名称字段余量，设备识别完全依赖厂商数据。
